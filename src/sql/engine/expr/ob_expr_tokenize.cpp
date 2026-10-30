/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/expr/ob_expr_tokenize.h"

#include <cstdlib>
#include <cstring>

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "storage/fts/dict/ob_ft_dict_hub.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{

namespace {

// tokenize(text, parser[, args]) is a pure function of its arguments: the parser
// name/properties are compiled into an immutable ObFTParserProperty, and the ik
// dictionaries are static text loaded once per process. Therefore the same input
// always produces the same JSON value. Bulk workloads that tokenize the same text
// repeatedly used to pay the whole "parse params -> pin dictionaries -> segment ->
// build JSON tree -> serialize" chain on every single call. Memoize the finished
// JSON binary in a small bounded per-thread table keyed by
// (parser name, parser properties, meta, output mode, fulltext).
constexpr int64_t TOKENIZE_CACHE_MAX_ENTRIES = 8;
constexpr int64_t TOKENIZE_CACHE_MAX_KEY_LEN = 4608;
constexpr int64_t TOKENIZE_CACHE_MAX_VALUE_LEN = 16 * 1024;

struct ObTokenizeCacheEntry
{
  ObTokenizeCacheEntry()
      : hash_(0), key_len_(0), value_len_(0), key_(nullptr), value_(nullptr), used_at_(0) {}
  uint64_t hash_;
  int64_t key_len_;
  int64_t value_len_;
  char *key_;
  char *value_;
  uint64_t used_at_;
};

class ObTokenizeResultCache
{
public:
  ObTokenizeResultCache() : used_at_(0) {}
  ~ObTokenizeResultCache()
  {
    for (int64_t i = 0; i < TOKENIZE_CACHE_MAX_ENTRIES; ++i) {
      release(entries_[i]);
    }
  }

  static ObTokenizeResultCache &instance()
  {
    // Leaked on purpose: the memo table owns plain malloc'd bytes, but keeping it
    // alive for the whole thread lifetime avoids any teardown-order coupling.
    static thread_local ObTokenizeResultCache *cache = new ObTokenizeResultCache();
    return *cache;
  }

  int get(const uint64_t hash, const char *key, const int64_t key_len, ObString &value)
  {
    int ret = OB_ENTRY_NOT_EXIST;
    for (int64_t i = 0; OB_ENTRY_NOT_EXIST == ret && i < TOKENIZE_CACHE_MAX_ENTRIES; ++i) {
      ObTokenizeCacheEntry &entry = entries_[i];
      if (OB_NOT_NULL(entry.key_)
          && entry.hash_ == hash
          && entry.key_len_ == key_len
          && 0 == ::memcmp(entry.key_, key, static_cast<size_t>(key_len))) {
        entry.used_at_ = ++used_at_;
        value.assign_ptr(entry.value_, static_cast<int32_t>(entry.value_len_));
        ret = OB_SUCCESS;
      }
    }
    return ret;
  }

  int put(const uint64_t hash, const char *key, const int64_t key_len, const ObString &value)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(key_len <= 0 || key_len > TOKENIZE_CACHE_MAX_KEY_LEN
                    || value.empty() || value.length() > TOKENIZE_CACHE_MAX_VALUE_LEN)) {
      // not worth/possible to cache, just skip
    } else {
      int64_t victim = -1;
      for (int64_t i = 0; victim < 0 && i < TOKENIZE_CACHE_MAX_ENTRIES; ++i) {
        if (OB_ISNULL(entries_[i].key_)) {
          victim = i;
        }
      }
      if (victim < 0) {
        victim = 0;
        for (int64_t i = 1; i < TOKENIZE_CACHE_MAX_ENTRIES; ++i) {
          if (entries_[i].used_at_ < entries_[victim].used_at_) {
            victim = i;
          }
        }
      }
      ObTokenizeCacheEntry &entry = entries_[victim];
      release(entry);
      char *key_buf = static_cast<char *>(::malloc(static_cast<size_t>(key_len)));
      char *value_buf = static_cast<char *>(::malloc(static_cast<size_t>(value.length())));
      if (OB_ISNULL(key_buf) || OB_ISNULL(value_buf)) {
        ::free(key_buf);
        ::free(value_buf);
      } else {
        ::memcpy(key_buf, key, static_cast<size_t>(key_len));
        ::memcpy(value_buf, value.ptr(), static_cast<size_t>(value.length()));
        entry.hash_ = hash;
        entry.key_len_ = key_len;
        entry.value_len_ = value.length();
        entry.key_ = key_buf;
        entry.value_ = value_buf;
        entry.used_at_ = ++used_at_;
      }
    }
    return ret;
  }

private:
  static void release(ObTokenizeCacheEntry &entry)
  {
    if (OB_NOT_NULL(entry.key_)) {
      ::free(entry.key_);
      entry.key_ = nullptr;
    }
    if (OB_NOT_NULL(entry.value_)) {
      ::free(entry.value_);
      entry.value_ = nullptr;
    }
    entry.hash_ = 0;
    entry.key_len_ = 0;
    entry.value_len_ = 0;
    entry.used_at_ = 0;
  }

private:
  ObTokenizeCacheEntry entries_[TOKENIZE_CACHE_MAX_ENTRIES];
  uint64_t used_at_;
};

inline uint64_t tokenize_cache_hash(const char *data, const int64_t len)
{
  uint64_t hash = 1469598103934665603ULL; // FNV-1a 64 bit
  for (int64_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(data[i]));
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Serialize the memo key: [meta+mode][parser name]\0[parser properties]\0[fulltext]
// Returns false when the payload does not fit the bounded stack buffer (skip caching).
bool build_tokenize_cache_key(const ObString &parser_name,
                              const ObString &properties,
                              const ObObjMeta &meta,
                              const ObString &fulltext,
                              const int output_mode,
                              const int64_t dict_epoch,
                              char *buf,
                              const int64_t buf_len,
                              int64_t &key_len)
{
  key_len = 0;
  const int64_t meta_desc_size = static_cast<int64_t>(sizeof(int64_t));
  const int64_t payload = 2 * meta_desc_size + parser_name.length() + properties.length()
                          + fulltext.length() + 2; // 2 x '\0' separators
  bool ok = false;
  if (OB_NOT_NULL(buf) && payload > 0 && payload <= buf_len) {
    int64_t pos = 0;
    const int64_t meta_desc = static_cast<int64_t>(meta.get_collation_type()) * 8 + output_mode;
    ::memcpy(buf + pos, &meta_desc, static_cast<size_t>(meta_desc_size));
    pos += meta_desc_size;
    ::memcpy(buf + pos, &dict_epoch, static_cast<size_t>(meta_desc_size));
    pos += meta_desc_size;
    if (parser_name.length() > 0) {
      ::memcpy(buf + pos, parser_name.ptr(), static_cast<size_t>(parser_name.length()));
      pos += parser_name.length();
    }
    buf[pos++] = '\0';
    if (properties.length() > 0) {
      ::memcpy(buf + pos, properties.ptr(), static_cast<size_t>(properties.length()));
      pos += properties.length();
    }
    buf[pos++] = '\0';
    if (fulltext.length() > 0) {
      ::memcpy(buf + pos, fulltext.ptr(), static_cast<size_t>(fulltext.length()));
      pos += fulltext.length();
    }
    key_len = pos;
    ok = true;
  }
  return ok;
}

} // anonymous namespace
ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else {
    char key_buf[TOKENIZE_CACHE_MAX_KEY_LEN];
    int64_t key_len = 0;
    // The memo key carries the dictionary epoch, so a rebuilt dictionary naturally
    // invalidates every entry instead of poisoning the table.
    const bool cacheable = build_tokenize_cache_key(param.parser_name_,
                                                       param.properties_,
                                                       param.meta_,
                                                       param.fulltext_,
                                                       static_cast<int>(param.output_mode_),
                                                       storage::ob_fts_dict_epoch(),
                                                       key_buf,
                                                       static_cast<int64_t>(sizeof(key_buf)),
                                                       key_len);
    ObTokenizeResultCache &cache = ObTokenizeResultCache::instance();
    const uint64_t cache_hash = cacheable ? tokenize_cache_hash(key_buf, key_len) : 0;
    ObString cached;
    if (cacheable && OB_SUCC(cache.get(cache_hash, key_buf, key_len, cached))) {
      if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, cached))) {
        LOG_WARN("failed to pack the memoized tokenize result", K(ret));
      }
    } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
    } else {
      ObString raw_binary;
      if (OB_FAIL(share::ObJsonWrapper::get_raw_binary(json_result, raw_binary, &temp_allocator))) {
        LOG_WARN("failed to serialize the tokenize result", K(ret));
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, raw_binary))) {
      } else if (cacheable) {
        // best effort: a failed memo put must never fail the query
        cache.put(cache_hash, key_buf, key_len, raw_binary);
      }
    }
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;


  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

namespace {
// The inner parser name is a pure function of the parser name the user wrote,
// but producing it walks the parser table with string compares and then
// serialises the result, on every single TOKENIZE evaluation.  Cache the
// mapping in fixed per-thread storage; the cached bytes live as long as the
// thread and the expression keeps using the name only inside one evaluation.
struct ObTokenizeParserNameSlot
{
  static const int64_t MAX_BYTES = 256;
  char key_[MAX_BYTES];
  char val_[MAX_BYTES];
  int64_t key_len_;
  int32_t val_len_;
  ObTokenizeParserNameSlot() : key_len_(-1), val_len_(0) { MEMSET(key_, 0, sizeof(key_)); }
};

struct ObTokenizeParserNameCache
{
  static const int64_t SLOT_CNT = 4;
  ObTokenizeParserNameSlot slots_[SLOT_CNT];
  int64_t next_;
  ObTokenizeParserNameCache() : next_(0) {}
};
} // namespace

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  static thread_local ObTokenizeParserNameCache name_cache;
  const int64_t in_len = input_str.length();
  const bool cacheable = in_len > 0 && in_len <= ObTokenizeParserNameSlot::MAX_BYTES;
  ObTokenizeParserNameSlot *hit = nullptr;

  if (cacheable) {
    for (int64_t i = 0; OB_ISNULL(hit) && i < ObTokenizeParserNameCache::SLOT_CNT; ++i) {
      ObTokenizeParserNameSlot &slot = name_cache.slots_[i];
      if (slot.key_len_ == in_len && 0 == MEMCMP(slot.key_, input_str.ptr(), in_len)) {
        hit = &slot;
      }
    }
  }

  if (nullptr != hit) {
    param.parser_name_ = ObString::make_string(hit->val_);
  } else {
    storage::ObFTParser parser;
    char *parser_name_buf = nullptr;
    if (OB_ISNULL(parser_name_buf
                  = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(parser.init(input_str))) {
    } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
    } else {
      param.parser_name_ = ObString::make_string(parser_name_buf);
      if (cacheable) {
        ObTokenizeParserNameSlot &slot = name_cache.slots_[name_cache.next_];
        name_cache.next_ = (name_cache.next_ + 1) % ObTokenizeParserNameCache::SLOT_CNT;
        MEMCPY(slot.key_, input_str.ptr(), in_len);
        const int64_t name_len = param.parser_name_.length();
        if (name_len + 1 <= ObTokenizeParserNameSlot::MAX_BYTES) {
          MEMCPY(slot.val_, param.parser_name_.ptr(), name_len);
          slot.val_[name_len] = '\0';
          slot.key_len_ = in_len;
          slot.val_len_ = static_cast<int32_t>(name_len);
        }
      }
    }
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

namespace {
// Rebuilding the parser properties is a JSON round trip whose result depends only
// on (parser name, properties) - both constants for a given statement.  Cache the
// last few results per thread in fixed storage so a repeated TOKENIZE call is a
// length check plus pointer assignment.  Fixed slots keep the cache bounded and
// allocation free, and a slot is only reused once all of its keys are compared.
struct ObTokenizePropSlot
{
  static const int64_t MAX_BYTES = 512;
  char key_[MAX_BYTES];
  char val_[MAX_BYTES];
  int64_t key_len_;
  int32_t val_len_;
  ObTokenizePropSlot() : key_len_(-1), val_len_(0) { MEMSET(key_, 0, sizeof(key_)); }
};

struct ObTokenizePropCache
{
  static const int64_t SLOT_CNT = 8;
  ObTokenizePropSlot slots_[SLOT_CNT];
  int64_t next_;
  ObTokenizePropCache() : next_(0) {}
};
} // namespace

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  static thread_local ObTokenizePropCache prop_cache;
  const int64_t name_len = parser_name_.length();
  const int64_t prop_len = properties.length();
  const int64_t total = name_len + prop_len;
  const bool cacheable = total > 0 && total <= ObTokenizePropSlot::MAX_BYTES;
  ObTokenizePropSlot *hit = nullptr;

  if (cacheable) {
    for (int64_t i = 0; OB_ISNULL(hit) && i < ObTokenizePropCache::SLOT_CNT; ++i) {
      ObTokenizePropSlot &slot = prop_cache.slots_[i];
      if (slot.key_len_ == total
          && 0 == MEMCMP(slot.key_, parser_name_.ptr(), name_len)
          && 0 == MEMCMP(slot.key_ + name_len, properties.ptr(), prop_len)) {
        hit = &slot;
      }
    }
  }

  if (nullptr != hit) {
    properties_.assign_ptr(hit->val_, hit->val_len_);
  } else {
    storage::ObFTParserJsonProps parser_properties;
    if (OB_FAIL(parser_properties.init())) {
    } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
    } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                               ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                               true))) {
    } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
    } else if (cacheable && properties_.length() <= ObTokenizePropSlot::MAX_BYTES) {
      ObTokenizePropSlot &slot = prop_cache.slots_[prop_cache.next_];
      prop_cache.next_ = (prop_cache.next_ + 1) % ObTokenizePropCache::SLOT_CNT;
      MEMCPY(slot.key_, parser_name_.ptr(), name_len);
      MEMCPY(slot.key_ + name_len, properties.ptr(), prop_len);
      MEMCPY(slot.val_, properties_.ptr(), properties_.length());
      slot.key_len_ = total;
      slot.val_len_ = properties_.length();
    }
  }

  return ret;
}

// The loader path below is only meaningful while the dictionary is changing.  Cache the
// last verified (epoch, collation) per thread: as long as the dictionary has not been
// rebuilt since, the loader lookup and in-transaction load cannot change the outcome.
struct ObTokenizeDicGate
{
  uint64_t epoch_;
  ObCollationType collation_;
  bool valid_;
  ObTokenizeDicGate() : epoch_(0), collation_(CS_TYPE_INVALID), valid_(false) {}
};

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  static thread_local ObTokenizeDicGate dic_gate;
  const uint64_t dic_epoch = storage::ob_fts_dict_epoch();
  if (dic_gate.valid_ && dic_gate.epoch_ == dic_epoch
      && dic_gate.collation_ == meta_.get_collation_type()) {
    // dictionary unchanged since the last successful check on this thread
    return OB_SUCCESS;
  }
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  if (OB_SUCC(ret)) {
    dic_gate.epoch_ = dic_epoch;
    dic_gate.collation_ = meta_.get_collation_type();
    dic_gate.valid_ = true;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

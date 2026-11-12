# MiniVectorDB

2026 数据库大赛加赛题：单机、单进程、单客户端的轻量向量数据库。
从零实现 SQL 解析、平坦表存储、精确向量检索、单事务与崩溃恢复，不依赖任何现成存储引擎或向量索引库。

---

## 1. 语言与版本

| 项 | 值 |
|---|---|
| 语言 | C++20 |
| 编译器 | g++ 13（Ubuntu 24.04 默认），也支持 clang 17+ |
| 构建 | CMake ≥ 3.16 |
| 依赖 | 仅标准库；OpenMP 可选（找到则并行算距离，找不到自动降级单线程） |
| 目标平台 | Linux x86_64 |

---

## 2. 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
# 产物：build/minivectordb
```

容器构建用仓库根目录的 `Dockerfile`（多阶段：构建阶段装 g++/cmake，运行阶段只留 libstdc++）：

```bash
docker build -t minivectordb:latest .
```

---

## 3. 启动入口

前台运行，不监听端口、不暴露服务：

```bash
minivectordb --data-dir /data
```

- `--data-dir <path>`：数据目录，默认 `/data`，也接受 `--data-dir=...`、环境变量 `DATA_DIR` / `MVDB_DATA_DIR`
- 其余参数（`--mode=ann`、`--vector-mode=ann` 等）一律忽略：本题 ann 与 exact 结果必须一致
- stdin：接收 SQL（**必须按分号切分语句**，见第 8 节）
- stdout：**每条语句恰好一行 JSON**（见 3.2），不混入任何日志
- stderr：日志（恢复进度、checkpoint、关停）

### 3.1 通信协议（JSONL，题包 STDIO_PROTOCOL.md）

判题器用 `docker run -i` 启动程序，通过 stdin/stdout 收发**一行一个 JSON 对象**：

```text
判题器 ── {"id":1,"op":"execute","sql":"...","search_mode":"exact"} ──> stdin
判题器 <── {"id":1,"ok":true,"columns":[...],"rows":[...]} ──────────── stdout
```

**启动握手**：恢复（快照 + WAL 重放）完成后，先输出一行并 flush：

```json
{"type":"ready"}
```

这个对象必须精确，不能多字段，不能是 `READY` 文本（那会导致 `invalid JSON response`）。

**执行成功**：

| 语句类型 | 响应 |
|---|---|
| 查询 | `{"id":N,"ok":true,"columns":["id"],"rows":[[1]]}`（空结果也带列名，`rows` 为 `[]`） |
| INSERT / UPDATE / DELETE | `{"id":N,"ok":true,"affected_rows":1}` |
| CREATE TABLE / DROP / BEGIN / COMMIT / ROLLBACK | `{"id":N,"ok":true}` |

**执行失败**（不终止进程，继续接收后续请求）：

```json
{"id":3,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key"}}
```

**关闭**：收到 `{"id":N,"op":"shutdown"}` → 回滚未提交事务、落盘、回 `{"id":N,"ok":true}`、以退出码 0 结束，此后 stdout 不再输出。

要点：

- `id` 原样回传（数字不能转成字符串），`ok` 必须是 JSON 布尔值
- 每个请求**恰好**一个响应，不发心跳、不发空行、不发提示符
- 每条响应后立即 flush
- stdout 只有协议报文；日志与诊断全部走 stderr
- 不输出 `NaN` / `Infinity`；向量投影是 JSON 数组，浮点用 `%.9g` 精确往返 float32
- SQL 按 JSON 字段整串下发，**不能再按 `;` 切分**（分号可能出现在字符串里）

自测时用 `MVDB_PROTOCOL=sql` 可退回纯 SQL 模式（旧的自测脚本用它）：

| 环境变量 | 作用 |
|---|---|
| （默认） | JSONL 协议，与判题器一致 |
| `MVDB_PROTOCOL=sql` | 纯 SQL 输入 / 单行 JSON 输出（仅本地自测） |
| `MVDB_QUIET=1` | stderr 只保留就绪与关停 |
| `MVDB_WIRE=text\|bare` | 纯 SQL 模式下的文本输出（调试） |

用官方工具验证（题包 `protocol_check/`）：

```bash
python3 protocol_check/run.py --image minivectordb:submit --handshake-only --output out/handshake
python3 protocol_check/run.py --image minivectordb:submit --output out/public
```

容器方式：

```bash
docker run --rm -i --network=none --cpus=2 --memory=2g -v mvd-data:/data minivectordb:latest
```

`[实]` 当前版本用官方工具跑过：handshake PASS、20 个公开用例 20/20 PASS（宿主进程与 docker 镜像各一遍，docker 参数为 `--network=none --cpus=2 --memory=2g --read-only --cap-drop=ALL`）。

### 3.2 错误语义名

`error.code` 是非空字符串即可（判题只校验 `ok:false` 与非空 code，`config.error_codes` 当前为空表）。本实现使用：

`duplicate_key` / `unsupported_sql` / `dimension_mismatch` / `invalid_vector` / `invalid_value` / `table_not_found` / `table_exists` / `column_not_found` / `value_too_long` / `syntax_error`

距离是欧氏距离（非平方），float32 存储后计算。

---

## 4. SQL 支持范围

### 4.1 DDL

```sql
CREATE TABLE t (
  id   BIGINT PRIMARY KEY,   -- 必须有且仅有一个 BIGINT 主键
  g    BIGINT,
  name VARCHAR(32),          -- 长度按 Unicode 字符数计
  v    VECTOR(128)           -- 每表最多一个向量列
);
CREATE VECTOR INDEX idx ON t(v);   -- 可选；不建也始终返回精确结果
DROP TABLE t;
```

### 4.2 DML

```sql
INSERT INTO t VALUES (1, 10, 'a', [0.1,0.2,...]), (2, 20, 'b', [...]);  -- 支持多行
UPDATE t SET name='x' WHERE id=1;
DELETE FROM t WHERE g=10;
```

### 4.3 查询

```sql
SELECT * FROM t;
SELECT id, name FROM t WHERE g >= 10 AND id < 100 ORDER BY id DESC LIMIT 5;
SELECT id, L2_DISTANCE(v, [0.1,0.2,...]) AS d FROM t ORDER BY d LIMIT 10;
SELECT id, L2_DISTANCE(v, [...]) AS d FROM t WHERE g=10 ORDER BY d LIMIT 10;  -- 过滤后取 TopK
```

支持：`=` `<` `<=` `>` `>=` `!=`、`AND`、列与列比较（`WHERE a > b`）、`ORDER BY`（多列、`ASC`/`DESC`）、`LIMIT`。
字符串比较按 Unicode 码点。

常量投影（连接探测常用）：`SELECT 1;`、`SELECT 1, 2;`、`SELECT 'x';`、`SELECT 1 FROM t;`。

### 4.4 事务

```sql
BEGIN;  ...  COMMIT;
BEGIN;  ...  ROLLBACK;
```

事务内可读到自己的修改；`ROLLBACK` 撤销全部修改（含 UPDATE）；语句执行失败时该语句自身的修改整体回滚。

### 4.5 不支持的语句（返回 `unsupported_sql`）

题面 2.1 要求「不支持的语句须返回约定错误，不能静默执行为其他含义」，因此 `ALTER TABLE`、`TRUNCATE`、
`RENAME`、`SET ...`、`USE ...`、`CREATE INDEX/DATABASE`、`DROP INDEX`、`LOCK`、`ANALYZE` 等合法但本题不做的
语句统一返回 `{"error":"unsupported_sql"}`；拼错的动词（如 `SELEC 1`）仍按语法错误返回 `syntax_error`。

### 4.6 明确不支持（均返回错误，不会静默执行）

`JOIN`、子查询、聚合（`COUNT`/`SUM`/`AVG`）、`OR`、`IS NULL`、`ALTER TABLE`、外键、嵌套事务、主键更新、`NULL` 字面量。
语句若含无法识别的尾部 token（如误写 `JOIN`），解析器会报语法错误而不是忽略。

---

## 5. 错误处理

JSON 协议下错误只给语义名：`{"error":"duplicate_key"}`（见 3.2）。文本协议（`MVDB_WIRE=text`）下为
`ERROR <code>: <message>`。语义名与内部错误码的对应：

| 语义名 | 内部码 | 含义 |
|---|---|---|
| `syntax_error` | 1064 | 语法错误 |
| `table_not_found` | 1146 | 表不存在 |
| `table_exists` | 1050 | 表已存在 |
| `column_not_found` | 1054 | 列不存在 |
| `duplicate_key` | 1062 | 主键重复 |
| `unsupported_sql` | 1235 | 不支持的语句（含主键更新、ALTER、SET） |
| `dimension_mismatch` | 1210 | 向量维度不符 |
| `invalid_vector` | 1211 | 向量值非法（NaN、Infinity、溢出、非数字） |
| `invalid_value` | 1213 | 普通列类型转换失败 |
| `value_too_long` | 1406 | VARCHAR 超长（按 Unicode 字符数） |

错误之后会话继续执行后续语句；失败的语句不留下任何部分修改。

---

## 6. 持久化与恢复

数据目录下两个文件：

| 文件 | 内容 |
|---|---|
| `snapshot.db` | 全量状态快照（紧凑二进制，含向量列；边序列化边写盘，不整份驻留内存） |
| `wal.log` | 快照之后的增量（已提交 SQL） |

- **提交语义**：题面 3.2 把崩溃模型限定为「进程被强制终止」，不模拟断电或磁盘损坏，因此提交时把
  WAL 写入内核页缓存即满足「已确认提交必须保留」；另加每 250ms 一次的节流 `fdatasync`，正常关闭必刷盘
- `MVDB_SYNC=always` 可退回「每条提交都 fsync」的保守模式（写入耗时约 2.2 倍）
- **自动 checkpoint**：WAL 超过 32 MiB 且无未提交事务时，落快照并截断 WAL（临时文件 + fsync + rename 原子替换）
- **崩溃恢复**：先读 `snapshot.db`，再重放 `wal.log`；恢复完成前不报告就绪。已确认提交的数据不丢；未提交事务不生效；
  恢复途中再次被强杀不会破坏状态（WAL 未被截断，下次重新重放）

---

## 7. 从空目录开始的完整示例

```bash
# 1) 准备空数据目录
rm -rf /tmp/mvdb-demo && mkdir -p /tmp/mvdb-demo

# 2) 写入 + 普通查询 + 向量检索
cat <<'SQL' | build/minivectordb --data-dir /tmp/mvdb-demo
CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, name VARCHAR(16), v VECTOR(3));
INSERT INTO t VALUES (1,1,'a',[1,0,0]),(2,1,'b',[0,1,0]),(3,2,'c',[0,0,1]);
SELECT id,name FROM t WHERE g=1 ORDER BY id;
SELECT id, L2_DISTANCE(v,[1,0,0]) AS d FROM t ORDER BY d LIMIT 2;
SQL

# 3) 事务提交
cat <<'SQL' | build/minivectordb --data-dir /tmp/mvdb-demo
BEGIN;
INSERT INTO t VALUES (4,2,'d',[1,1,1]);
COMMIT;
SELECT id FROM t ORDER BY id;
SQL

# 4) 事务回滚
cat <<'SQL' | build/minivectordb --data-dir /tmp/mvdb-demo
BEGIN;
INSERT INTO t VALUES (5,9,'e',[9,9,9]);
ROLLBACK;
SELECT id FROM t ORDER BY id;   -- 不应包含 5
SQL

# 5) 重启后一致
echo 'SELECT id FROM t ORDER BY id;' | build/minivectordb --data-dir /tmp/mvdb-demo
```

预期输出（默认 JSON 协议，一条语句一行）：

```text
第 2 步：{}
        {"affected_rows":3}
        {"columns":["id","name"],"rows":[[1,"a"],[2,"b"]]}
        {"columns":["id","d"],"rows":[[1,0.0],[2,1.4142135623730951]]}
第 3 步：{} {"affected_rows":1} {} {"columns":["id"],"rows":[[1],[2],[3],[4]]}
第 4 步：{} {"affected_rows":1} {} {"columns":["id"],"rows":[[1],[2],[3],[4]]}
第 5 步：{"columns":["id"],"rows":[[1],[2],[3],[4]]}
```

---

## 8. 驱动约束（重要）

**必须按分号切分语句后再喂入。** 程序以 `;` 作为语句结束标志；若发送不含分号的内容（例如单独一行注释），程序会继续等待语句结束，表现为无输出。

注释（`--`、`#`、`/* */`）可在语句内部或前后，它们参与同一次输入、由分号切分，不会被误当语句边界。

---

## 9. 已知限制

- 存储为全内存 + 快照/WAL。实测 30 万行 × 128 维：峰值 RSS 387 MB、重启 0.65s、重启前后 TopK 完全一致；
  据此推算 2 GiB 容器约可承载 100–130 万行 × 128 维，超过需改为 mmap/分块加载
- 未实现真正的近似索引。`CREATE VECTOR INDEX` 仅登记，检索始终精确（exact）。`--mode ann` 被接受但仍返回精确结果（Recall 恒为 1.0，满足 ≥0.95 要求）
- 单客户端、最多一个显式事务（题面限定），未实现并发控制
- 不支持 `NULL`、`JOIN`、聚合、子查询、`ALTER TABLE`、外键、嵌套事务、主键更新
- 性能（10,000 行 × 128 维 / 1,000 次 TopK K=10 / 一半带过滤，2 核）：写入 0.57s、查询 0.43s（约 2,300 qps），
  与暴力真值比对 Recall@10 = 1.000

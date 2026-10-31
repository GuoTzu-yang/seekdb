/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* Reused per-index scratch: seekdb issues one vector per search, so
   * re-allocating the device buffers and the search params every call costs
   * more than the search itself at these query counts. */
  cuvsCagraSearchParams_t sp;
  float*    qd;  size_t qd_cap;   /* floats   */
  unsigned* nd;  size_t nd_cap;   /* elements */
  float*    dd;  size_t dd_cap;   /* floats   */
} sk_cuvs_index;

static int sk_reserve(cuvsResources_t res, void** p, size_t* cap, size_t need, size_t elem)
{
  if (*cap >= need) { return 0; }
  if (*p != NULL) { cuvsRMMFree(res, *p, *cap * elem); *p = NULL; *cap = 0; }
  void* q = NULL;
  if (cuvsRMMAlloc(res, &q, need * elem) != CUVS_SUCCESS) { return 1; }
  *p = q; *cap = need;
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  // The wide search below makes a smaller NN-descent budget safe: 128-d
  // 128/24 builds ~3.4x faster and 256-d 160/32 ~2.8x, with paired recall
  // still inside the 0.005 window (width 4 needed 30/45 for the same recall).
  const bool low_dim = (dim <= 128);
  h->ip->intermediate_graph_degree = low_dim ? 128 : 160;
  h->ip->nn_descent_niter = low_dim ? 24 : 32;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  if (sk_reserve(h->res, (void**)&h->qd, &h->qd_cap, (size_t)nq*h->dim, sizeof(float)) ||
      sk_reserve(h->res, (void**)&h->nd, &h->nd_cap, (size_t)nq*topk, sizeof(unsigned)) ||
      sk_reserve(h->res, (void**)&h->dd, &h->dd_cap, (size_t)nq*topk, sizeof(float))) {
    rc = 10; goto done;
  }
  float* qd = h->qd; unsigned* nd = h->nd; float* dd = h->dd;
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (h->sp == NULL && cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      if (h->sp->itopk_size < required_itopk) h->sp->itopk_size = required_itopk;
      // hipVS single-CTA search caps itopk at 512; width 32 is what fits the
      // hashmap limit. It improves recall and QPS at the same time.
      h->sp->search_width = 32;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

int seekdb_cuvs_copy_dataset(void* handle, float* out, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !out || n <= 0 || dim <= 0) return 1;
  DLManagedTensor ds;
  memset(&ds, 0, sizeof(ds));
  if (cuvsCagraIndexGetDataset(h->idx, &ds) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] copy_dataset: get failed: %s\n", cuvsGetLastErrorText());
    return 2;
  }
  int rc = 0;
  if (ds.dl_tensor.ndim != 2 || ds.dl_tensor.shape == NULL ||
      ds.dl_tensor.shape[0] != (int64_t)n || ds.dl_tensor.shape[1] != (int64_t)dim) {
    rc = 3;
  } else if (cudaMemcpy(out, ds.dl_tensor.data, sizeof(float) * (size_t)n * (size_t)dim,
                        cudaMemcpyDefault) != cudaSuccess) {
    rc = 4;
  }
  if (ds.deleter != NULL) { ds.deleter(&ds); }
  if (rc != 0) { fprintf(stderr, "[seekdb_cuvs] copy_dataset failed rc=%d\n", rc); }
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  if (h->qd) cuvsRMMFree(h->res, h->qd, h->qd_cap * sizeof(float));
  if (h->nd) cuvsRMMFree(h->res, h->nd, h->nd_cap * sizeof(unsigned));
  if (h->dd) cuvsRMMFree(h->res, h->dd, h->dd_cap * sizeof(float));
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}

#include "dftl_bm.h"
#include "../lower/lower.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "demand.h"
#include <glib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
extern volatile int gc_inflight;
void dftl_show_sblk_state(block_mgr_t *self, int pt_num);
bm_env_t bm_env = {
    .grain_cnt = GRAIN_PER_PAGE * _NOP, .valid_bitmap = NULL, .oob = NULL};
#ifdef DATA_SEGREGATION
bm_superblock_t *dftl_get_stream_sblk(block_mgr_t *self, lpa_t lpa,
                                      bool is_reserve);
#endif
block_mgr_t bm = {
    .env = &bm_env,
    .create = dftl_bm_init,
    .destroy = dftl_bm_free,
    .isvalid_page = dftl_isvalid_page,
    .validate_page = dftl_validate_page,
    .invalidate_page = dftl_invalidate_page,
    .isvalid_grain = dftl_isvalid_grain,
    .validate_grain = dftl_validate_grain,
    .invalidate_grain = dftl_invalidate_grain,
    .isgc_needed = dftl_isgc_needed,
    .check_full = dftl_check_full,
    .get_page_num = dftl_get_page_num,
    .get_active_superblock = dftl_get_active_sblk,
    .get_gc_target = dftl_get_gc_target,
    .trim_segment = dftl_trim_segment,
    .change_reserve = dftl_change_reserve,
    .get_oob = dftl_bm_get_oob,
    .set_oob = dftl_bm_set_oob,
    .show_sblk_state = dftl_show_sblk_state,
#ifdef DATA_SEGREGATION
    .get_stream_superblock = dftl_get_stream_sblk,

#endif
};
block_mgr_t *pbm = &bm;

void dftl_bm_init(block_mgr_t *self) {
  bm_env_t *env = self->env;
  env->valid_bitmap = (bool *)g_malloc0(sizeof(bool) * env->grain_cnt);
  env->oob = (bm_oob_t *)g_malloc0(sizeof(bm_oob_t) * env->grain_cnt);
  env->sblk = (bm_superblock_t *)g_malloc0(sizeof(bm_superblock_t) * _NOS);
  for (int i = 0; i < _NOS; i++) {
    *(uint32_t *)(&(env->sblk[i])) = i; // sblk->index = i
#ifdef DATA_SEGREGATION
    env->sblk[i].is_owned = false;
    env->sblk[i].stream_idx = -1;
    env->sblk[i].is_rsv = false;
#endif
  }
  env->part_num = 2;
  env->part = (bm_part_ext_t *)g_malloc0(sizeof(bm_part_ext_t) * env->part_num);
  env->part[0].s_sblk = 0;
  env->part[0].e_sblk = _NOS / 20;
  env->part[0].free_sblk_cnt = _NOS / 20 + 1;
  env->part[0].sblk_rsv = 0;
  env->part[0].active_sblk = -1;
  env->part[1].s_sblk = _NOS / 20 + 1;
  env->part[1].e_sblk = _NOS - 1;
  env->part[1].free_sblk_cnt = _NOS - (_NOS / 20 + 1);
  env->part[1].sblk_rsv = _NOS / 20 + 1;
  env->part[1].active_sblk = -1;
#ifdef DATA_SEGREGATION
  /* 仿单流 dftl_page_init：
   *   单流：d_reserve = get_active_superblock(true); d_active = NULL
   *   DS  ：准备 MAX_GC_STREAM 个 d_reserve 进 rsv 池，再各流拿 1 个 active。
   *
   * 计数约束：
   *   - 先拿 MAX_GC_STREAM 个 d_reserve（从 free 池）：free_sblk_cnt -= MAX_GC_STREAM,
   *     rsv_sblk_cnt += MAX_GC_STREAM, 每个 sblk 标 is_rsv=true。
   *   - 再各流从 free 池拿 1 个 active：free_sblk_cnt -= MAX_GC_STREAM,
   *     sblk 标 is_owned=true, stream_idx=i。 */
  env->part[1].rsv_sblk_cnt = 0;
  env->stream = (bm_stream_manager_t *)g_malloc0(sizeof(bm_stream_manager_t) *
                                                MAX_GC_STREAM);
  /* 阶段 1：从 free 池拿 MAX_GC_STREAM 个 sblk 进 d_reserve 池 */
  for (int i = 0; i < MAX_GC_STREAM; i++) {
    bm_superblock_t *sblk =
        self->get_active_superblock(self, DATA_S, /*is_reserve=*/false);
    ftl_assert(sblk != NULL);
    sblk->is_owned = false;
    sblk->stream_idx = -1;
    sblk->is_rsv = true;
    env->part[1].rsv_sblk_cnt++;
  }
  ftl_assert(env->part[1].rsv_sblk_cnt == MAX_GC_STREAM);
  /* 阶段 2：各流从 free 池拿 1 个 sblk 做 active */
  for (int i = 0; i < MAX_GC_STREAM; i++) {
    bm_superblock_t *sblk =
        self->get_active_superblock(self, DATA_S, /*is_reserve=*/false);
    ftl_assert(sblk != NULL);
    ftl_assert(!sblk->is_rsv);
    sblk->is_owned = true;
    sblk->stream_idx = i;
    /* init 不主动调 get_page_num。grain_remain = 0 强制第一个 wb_entry 触发
     * dp_alloc 开段。active_ppa / flush_ppa 在 dp_alloc 之后才确定。 */
    env->stream[i].idx = i;
    env->stream[i].active_sblk = sblk;
    env->stream[i].active_sblk_offset = sblk->wp_offt;
    env->stream[i].active_ppa = SBLK_OFFT2PPA(sblk, sblk->wp_offt);
    env->stream[i].flush_ppa = env->stream[i].active_ppa;
    env->stream[i].flush_page = 0;
    env->stream[i].grain_remain = 0; /* 强制首个 wb_entry 触发 dp_alloc */
    env->stream[i].page_remain = SBLK_END - sblk->wp_offt;
  }
#endif
}

void dftl_bm_free(block_mgr_t *self) {
  bm_env_t *env = self->env;
  g_free(env->oob);
  g_free(env->valid_bitmap);
  g_free(env->part);
  g_free(env->sblk);
#ifdef DATA_SEGREGATION
  g_free(env->stream);
  env->stream = NULL;
#endif
  env->oob = NULL;
  env->valid_bitmap = NULL;
  env->part = NULL;
  env->sblk = NULL;
}

bool dftl_isvalid_page(block_mgr_t *self, ppa_t ppa) {
  bm_env_t *env = self->env;
  bool state = env->valid_bitmap[ppa * GRAIN_PER_PAGE];
  for (int i = 0; i < GRAIN_PER_PAGE; i++) {
    ftl_assert(state == env->valid_bitmap[ppa * GRAIN_PER_PAGE + i]);
  }
  return state;
}

void dftl_validate_page(block_mgr_t *self, ppa_t ppa) {
  bm_env_t *env = self->env;
  for (int i = 0; i < GRAIN_PER_PAGE; i++) {
    ftl_assert(env->valid_bitmap[ppa * GRAIN_PER_PAGE + i] == false);
    env->valid_bitmap[ppa * GRAIN_PER_PAGE + i] = true;
    env->sblk[SBLK_IDX(ppa)].valid_cnt++;
  }
}

void dftl_invalidate_page(block_mgr_t *self, ppa_t ppa) {
  bm_env_t *env = self->env;
  for (int i = 0; i < GRAIN_PER_PAGE; i++) {
    ftl_assert(env->valid_bitmap[ppa * GRAIN_PER_PAGE + i] == true);
    env->valid_bitmap[ppa * GRAIN_PER_PAGE + i] = false;
    env->sblk[SBLK_IDX(ppa)].valid_cnt--;
  }
}

bool dftl_isvalid_grain(block_mgr_t *self, ppa_t grain) {
  bm_env_t *env = self->env;
  return env->valid_bitmap[grain];
}

void dftl_validate_grain(block_mgr_t *self, ppa_t grain) {
  bm_env_t *env = self->env;
  ftl_assert(env->valid_bitmap[grain] == false);
  env->valid_bitmap[grain] = true;
  env->sblk[SBLK_IDX(grain / GRAIN_PER_PAGE)].valid_cnt++;
  if (env->sblk[SBLK_IDX(grain / GRAIN_PER_PAGE)].valid_cnt >
      _PPS * GRAIN_PER_PAGE)
    abort();
}

void dftl_invalidate_grain(block_mgr_t *self, ppa_t grain) {
  bm_env_t *env = self->env;
  ftl_assert(env->valid_bitmap[grain] == true);
  env->valid_bitmap[grain] = false;
  env->sblk[SBLK_IDX(grain / GRAIN_PER_PAGE)].valid_cnt--;
}

bm_oob_t *dftl_bm_get_oob(block_mgr_t *self, ppa_t grain) {
  bm_env_t *env = self->env;
  return &env->oob[grain];
}

void dftl_bm_set_oob(block_mgr_t *self, ppa_t grain, bm_oob_t *oob) {
  bm_env_t *env = self->env;
  env->oob[grain] = *oob;
}

bool dftl_check_full(block_mgr_t *self, bm_superblock_t *sblk) {
  if (sblk == NULL) {
    ftl_err("[check_full] sblk=NULL called, ret=%p ret2=%p\n",
            (void*)__builtin_return_address(0),
            (void*)__builtin_return_address(1));
    self->show_sblk_state(self, DATA_S);
    abort();
  }
  return sblk->wp_offt >= SBLK_END;
}

ppa_t dftl_get_page_num(block_mgr_t *self, bm_superblock_t *sblk) {
  ftl_assert(!self->check_full(self, sblk));
  ftl_assert(sblk->wp_offt < SBLK_END);
  return SBLK_OFFT2PPA(sblk, sblk->wp_offt++);
}

bm_superblock_t *dftl_get_active_sblk(block_mgr_t *self, int pt_num,
                                      bool isreserve) {
  bm_env_t *env = self->env;
  ftl_assert(0 <= pt_num && pt_num < env->part_num);
  bm_part_ext_t *pt = &env->part[pt_num];
#ifdef DATA_SEGREGATION
  if (isreserve && pt_num == DATA_S) {
    /* DS 模式 isreserve=true：仅在 dftl_page_init 调一次（拿 d_reserve）。
     * 从 rsv 池拿。is_rsv && !is_owned && not full。 */
    for (int i = pt->s_sblk; i <= pt->e_sblk; i++) {
      if (env->sblk[i].is_rsv == true &&
          !self->check_full(self, &env->sblk[i]) &&
          env->sblk[i].is_owned == false) {
        pt->active_sblk = i;
        break;
      }
    }
    if (pt->active_sblk == -1) {
      ftl_err("dftl_get_active_sblk rsv: no rsv sblk (pt=%d)\n", pt_num);
    }
    return &env->sblk[pt->active_sblk];
  }
#endif
  if (isreserve) {
    ftl_assert(env->sblk[pt->sblk_rsv].wp_offt == 0 &&
               env->sblk[pt->sblk_rsv].valid_cnt == 0);
    return &env->sblk[pt->sblk_rsv];
  }

  if (pt->active_sblk == -1) {
    for (int i = pt->s_sblk; i <= pt->e_sblk; i++) {
      if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i])) {
        pt->active_sblk = i;
        break;
      }
    }
  }
  if (pt_num == DATA_S) {
    uint32_t start_sblk = pt->active_sblk;
    pt->active_sblk = -1;
    /* 搜索 active sblk 后面的空闲 superblock。
     * DS 模式：free 池 = !is_rsv && !is_owned && not full。 */
    for (int i = start_sblk + 1; i <= pt->e_sblk; i++) {
#ifdef DATA_SEGREGATION
      if (env->sblk[i].is_rsv == false &&
          !self->check_full(self, &env->sblk[i]) &&
          env->sblk[i].is_owned == false)
#else

      if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]))
#endif
      {
        pt->active_sblk = i;
        break;
      }
    }
    /* 没找到时从头搜索。 */
    if (pt->active_sblk == -1) {
      for (int i = pt->s_sblk; i < start_sblk; i++) {
#ifdef DATA_SEGREGATION
        if (env->sblk[i].is_rsv == false &&
            !self->check_full(self, &env->sblk[i]) &&
            env->sblk[i].is_owned == false)
#else
        if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]))
#endif
        {
          pt->active_sblk = i;
          break;
        }
      }
    }
#ifdef DATA_SEGREGATION
    if (pt->active_sblk == -1) {
      ftl_err("dftl_get_active_sblk: no free sblk (pt=%d)\n", pt_num);
    }
#endif
  } else {
    uint32_t start_sblk = pt->active_sblk;
    pt->active_sblk = -1;
    // 搜索active sblk后面的空闲superblock
    for (int i = start_sblk + 1; i <= pt->e_sblk; i++) {
      if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i])) {
        pt->active_sblk = i;
        break;
      }
    }
    // 没找到时，从头搜索
    if (pt->active_sblk == -1) {
      for (int i = pt->s_sblk; i < start_sblk; i++) {
        if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i])) {
          pt->active_sblk = i;
          break;
        }
      }
    }
  }
  if (pt->active_sblk == -1) {
    return NULL;
  }
  if (env->sblk[pt->active_sblk].wp_offt == 0) {
    ftl_assert(pt->free_sblk_cnt > 0);
    pt->free_sblk_cnt--;
  }
  return &env->sblk[pt->active_sblk];
}

#ifdef DATA_SEGREGATION
/* 切流逻辑：仿单流 dftl_get_active_sblk(false) 非 reserve 路径。
 *  - 旧 sblk：仅 is_owned=false（裸满等 GC 选中），不动 is_rsv/free 计数
 *  - 新 sblk：从 free 池拿（!is_rsv && !is_owned && !full），跳过 d_reserve
 *  - stream_idx 保留在旧 sblk 上：GC 用它决定 valid grain 回写到哪个流
 *
 * 计数约束：
 *   - 切流前：free_sblk_cnt + rsv_sblk_cnt + owned_cnt + 裸满块数 = _NOS-DATA_S
 *   - 切流：旧 owned_cnt--（is_owned=false），新 owned_cnt++（is_owned=true）
 *   - 新 sblk 从 free 池拿：free_sblk_cnt--（get_active_superblock 内部）
 *   - d_reserve 池不被切流路径消费（仅 GC 完成后水位维护会涉及）
 */
bm_superblock_t *dftl_get_stream_sblk(block_mgr_t *self, lpa_t stream_idx,
                                      bool is_reserve) {
  (void)is_reserve;
  bm_env_t *env = self->env;
  if (unlikely(stream_idx >= MAX_GC_STREAM)) {
    ftl_err("invalid stream_idx=%u, MAX_GC_STREAM=%d\n", (uint32_t)stream_idx,
            MAX_GC_STREAM);
    abort();
  }
  int idx = stream_idx;
  bm_stream_manager_t *stream = &env->stream[idx];

  bool need_switch =
      (stream->active_sblk == NULL) ||
      (self->check_full(self, stream->active_sblk)) ||
      (!stream->active_sblk->is_owned ||
       stream->active_sblk->stream_idx != idx);

  if (need_switch) {
    bm_superblock_t *old_sblk = stream->active_sblk;
    /* 旧 sblk：仅 is_owned=false，裸等 GC（仿单流切流） */
    if (old_sblk != NULL) {
      old_sblk->is_owned = false;
    }

    /* 新 sblk：从 free 池拿（仿单流 dftl_get_active_sblk(false)，跳过 d_reserve） */
    stream->active_sblk =
        self->get_active_superblock(self, DATA_S, /*is_reserve=*/false);
    if (unlikely(stream->active_sblk == NULL)) {
      ftl_err("dftl_get_stream_sblk: no free sblk for stream=%d\n", idx);
      self->show_sblk_state(self, DATA_S);
      abort();
    }
    ftl_assert(!stream->active_sblk->is_rsv);
    stream->active_sblk->is_owned = true;
    stream->active_sblk->stream_idx = idx;

    /* 切流时不主动调 get_page_num。grain_remain=0 强制下一个 wb_entry 触发
     * dp_alloc 开新段。 */
    stream->active_sblk_offset = stream->active_sblk->wp_offt;
    stream->active_ppa = SBLK_OFFT2PPA(stream->active_sblk, stream->active_sblk->wp_offt);
    stream->flush_ppa = stream->active_ppa;
    stream->flush_page = 0;
    stream->grain_remain = 0;
    stream->page_remain = SBLK_END - stream->active_sblk->wp_offt;
  }
  return stream->active_sblk;
}
#endif
void dftl_show_sblk_state(block_mgr_t *self, int pt_num) {
  bm_env_t *env = self->env;
  ftl_assert(0 <= pt_num && pt_num < env->part_num);
  bm_part_ext_t *pt = &env->part[pt_num];
  bm_superblock_t *target = NULL;
  uint32_t min_valid_cnt = UINT32_MAX;
#ifdef DATA_SEGREGATION
  ftl_log("------------sblk state------------- (free=%u, rsv=%u)\n",
          pt->free_sblk_cnt, pt->rsv_sblk_cnt);
  for (int i = pt->s_sblk; i <= pt->e_sblk; i++) {
    ftl_log("sblk[%d]: stream=%d, start_ppa=%llu, is_rsv=%d, is_owned=%d, "
            "is_full=%d, valid_cnt=%u, wp=%d\n",
            i, env->sblk[i].stream_idx,
            (unsigned long long)(env->sblk[i].index * _PPS),
            (env->sblk[i].is_rsv == true), (env->sblk[i].is_owned == true),
            self->check_full(self, &env->sblk[i]), env->sblk[i].valid_cnt,
            env->sblk[i].wp_offt);
  }
  ftl_log("------------stream state-------------\n");
  for (int i = 0; i < MAX_GC_STREAM; i++) {
    bm_env_t *env = self->env;
    bm_stream_manager_t *stream = &env->stream[i];
    ftl_log("stream[%d] (sblk[%u]): page_remain=%u grain_remain=%u "
            "active_ppa=%llu flush_ppa=%llu flush_page=%u sblk_wp=%u\n",
            stream->idx, stream->active_sblk->index, stream->page_remain,
            stream->grain_remain, (unsigned long long)stream->active_ppa,
            (unsigned long long)stream->flush_ppa, stream->flush_page,
            stream->active_sblk->wp_offt);
  }

#else
  for (int i = pt->s_sblk; i <= pt->e_sblk; i++) {
    // 打印包含 is_flying 状态的完整信息
    ftl_log("sblk[%d]: is_rsv=%d, is_full=%d, valid_cnt=%u, wp=%d\n", i,
            (i == pt->sblk_rsv), self->check_full(self, &env->sblk[i]),
            env->sblk[i].valid_cnt, env->sblk[i].wp_offt);
  }
#endif
}
bool dftl_isgc_needed(block_mgr_t *self, int pt_num) {
  bm_env_t *env = self->env;
  ftl_assert(0 <= pt_num && pt_num < env->part_num);
  bm_part_ext_t *pt = &env->part[pt_num];
#ifdef DATA_SEGREGATION
  /* DS 模式：仅 DATA 走新阈值（MAX_GC_STREAM+1=3）
   * MAP 仍走单流阈值（free<=1），保持原 MAP GC 触发行为 */
  if (pt_num == DATA_S) {
    return (pt->free_sblk_cnt <= MAX_GC_STREAM + 1) ? 1 : 0;
  }
#endif
  if (pt->free_sblk_cnt <= 1) {
    return 1; // reserved 1
  } else {
    return 0;
  }
}

bm_superblock_t *dftl_get_gc_target(block_mgr_t *self, int pt_num,
                                    int stream_idx) {
  bm_env_t *env = self->env;
  ftl_assert(0 <= pt_num && pt_num < env->part_num);
  bm_part_ext_t *pt = &env->part[pt_num];
  bm_superblock_t *target = NULL;
  uint32_t min_valid_cnt = UINT32_MAX;
#ifdef DATA_SEGREGATION
  if (pt_num == DATA_S) {
    /* DS 模式 GC 候选：仿单流跳过 reserved 选 valid 最小的 full sblk。
     *  - is_owned=true：被某流持有写入，跳过
     *  - is_rsv=true：d_reserve 池中的 sblk（切流备用的占位），跳过
     *  - 其它 is_full=1 && is_owned=false && is_rsv=false：候选 victim */
    for (int i = pt->s_sblk; i <= pt->e_sblk; i++) {
      if (env->sblk[i].is_owned == true)
        continue;
      if (env->sblk[i].is_rsv == true)
        continue;
      /* Prefer same-stream victim to keep locality during GC. */
      if (env->sblk[i].stream_idx == stream_idx &&
          self->check_full(self, &env->sblk[i]) &&
          env->sblk[i].valid_cnt < min_valid_cnt) {
        target = &env->sblk[i];
        min_valid_cnt = env->sblk[i].valid_cnt;
      }
    }
    /* Fallback: pick global full victim (not owned, not rsv). */
    if (target == NULL) {
      for (int i = pt->s_sblk; i <= pt->e_sblk; i++) {
        if (env->sblk[i].is_owned == true)
          continue;
        if (env->sblk[i].is_rsv == true)
          continue;
        if (self->check_full(self, &env->sblk[i]) &&
            env->sblk[i].valid_cnt < min_valid_cnt) {
          target = &env->sblk[i];
          min_valid_cnt = env->sblk[i].valid_cnt;
        }
      }
    }
  } else {
    for (int i = pt->s_sblk; i <= pt->e_sblk; i++) {
      if (i != pt->sblk_rsv && self->check_full(self, &env->sblk[i]) &&
          env->sblk[i].valid_cnt < min_valid_cnt) {
        target = &env->sblk[i];
        min_valid_cnt = env->sblk[i].valid_cnt;
      }
    }
  }
#else
  for (int i = pt->s_sblk; i <= pt->e_sblk; i++) {
    if (i != pt->sblk_rsv && self->check_full(self, &env->sblk[i]) &&
        env->sblk[i].valid_cnt < min_valid_cnt) {
      target = &env->sblk[i];
      min_valid_cnt = env->sblk[i].valid_cnt;
    }
  }
#endif
  if (target == NULL || min_valid_cnt == UINT32_MAX) {
    ftl_err("No GC target found (pt=%d, stream=%d)\n", pt_num, stream_idx);
    abort();
  }
  if (target->valid_cnt >= target->wp_offt * GRAIN_PER_PAGE) {
    ftl_err("No valid superblock for GC\n");
    abort();
  }
  return target;
}

void dftl_trim_segment(block_mgr_t *self, bm_superblock_t *sblk,
                       lower_info *li) {
  ftl_assert(sblk != NULL);
  li->trim_block(SBLK_OFFT2PPA(sblk, 0));
  sblk->wp_offt = 0;
  sblk->valid_cnt = 0;
  memset(&self->env->oob[(uint64_t)SBLK_OFFT2PPA(sblk, 0) * GRAIN_PER_PAGE], 0,
         sizeof(bm_oob_t) * _PPS * GRAIN_PER_PAGE);
  /* 重要：trim_segment 是 MAP/DATA 公共函数，保持原单流行为（仅清零元数据）
   * DS 模式下额外的 rsv 池维护放到调用方 dpage_gc_dvalue 内部（DATA 专属） */
#ifdef DEBUG_FTL
  for (int i = 0; i < SBLK_END; i++) {
    ftl_assert(!self->isvalid_page(self, SBLK_OFFT2PPA(sblk, i)));
  }
#endif
}

bm_superblock_t *dftl_change_reserve(block_mgr_t *self, int pt_num,
                                     bm_superblock_t *reserve) {
  bm_env_t *env = self->env;
  ftl_assert(0 <= pt_num && pt_num < env->part_num);
  bm_part_ext_t *pt = &env->part[pt_num];
  if (reserve != NULL) {
    pt->sblk_rsv = reserve->index;
  } else {
    pt->sblk_rsv = -1;
    ftl_err("No reserve superblock\n");
    abort();
  }
  return reserve;
}

void bm_show_gc_cnt_stats(block_mgr_t *bm) {
  bm_env_t *env = bm->env;
  uint64_t total_gc = 0;
  ftl_log("========== GC Count per SBLK ==========\n");
  for (int i = 0; i < (int)env->part_num; i++) {
    bm_part_ext_t *pt = &env->part[i];
    const char *pt_name = (i == MAP_S) ? "MAP" : "DATA";
    ftl_log("  partition %s [%u-%u]:\n", pt_name, pt->s_sblk, pt->e_sblk);
    for (uint32_t j = pt->s_sblk; j <= pt->e_sblk; j++) {
      if (bm->env->sblk[j].gc_cnt > 0) {
        ftl_log("    sblk[%u]: gc_cnt=%lu\n", j, (unsigned long)bm->env->sblk[j].gc_cnt);
        total_gc += bm->env->sblk[j].gc_cnt;
      }
    }
  }
  ftl_log("  total GC erase count: %lu\n", (unsigned long)total_gc);
  ftl_log("========================================\n");
}
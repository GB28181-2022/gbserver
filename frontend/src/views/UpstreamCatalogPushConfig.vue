<!--
  上级平台推送范围：布局对齐目录编组「本机编组」；左树勾选目录子树，右表对挂载通道可单独排除
-->
<template>
  <div class="page upstream-push-config">
    <header class="page-header push-header">
      <div class="push-header-left">
        <el-button class="back-btn" @click="goBack">返回</el-button>
        <h2 class="page-title push-page-title">上级推送范围</h2>
      </div>
    </header>

    <el-card v-loading="platformLoading" shadow="hover" class="info-card">
      <template #header>
        <span>上级平台</span>
      </template>
      <el-descriptions v-if="platform" :column="2" border size="small">
        <el-descriptions-item label="名称">{{ platform.name }}</el-descriptions-item>
        <el-descriptions-item label="上级国标 ID">{{ platform.gbId }}</el-descriptions-item>
        <el-descriptions-item label="信令地址">{{ platform.sipIp }}:{{ platform.sipPort }}</el-descriptions-item>
        <el-descriptions-item label="传输">{{ platform.transport }}</el-descriptions-item>
        <el-descriptions-item label="启用">
          <el-tag :type="platform.enabled ? 'success' : 'info'" size="small">
            {{ platform.enabled ? '是' : '否' }}
          </el-tag>
        </el-descriptions-item>
        <el-descriptions-item label="状态">
          <template v-if="!platform.enabled">
            <el-tag type="info" size="small">未启用</el-tag>
          </template>
          <template v-else>
            <el-tag :type="platform.online ? 'success' : 'danger'" size="small">
              {{ platform.online ? '在线' : '离线' }}
            </el-tag>
          </template>
        </el-descriptions-item>
      </el-descriptions>
      <div v-else class="empty-tip">加载中或无效的上级平台。</div>
    </el-card>

    <p class="scope-hint">
      与「目录编组」同一棵本机树。勾选左侧<strong>目录</strong>即圈定子树内全部通道默认推送；在右侧挂载表中可<strong>取消个别通道</strong>（写入排除列表）。须点击<strong>保存范围</strong>后才会持久化，下次进入自动恢复。对上级上报的 DeviceID 为编组国标（目录/通道）。
    </p>

    <div class="catalog-layout">
      <section class="catalog-left">
        <el-card v-loading="treeLoading" shadow="hover">
          <template #header>
            <div class="header-row">
              <span>编组树（勾选推送范围）</span>
              <div class="tree-actions">
                <el-button size="small" :disabled="!treeData.length" @click="expandAll">展开全部</el-button>
                <el-button size="small" :disabled="!treeData.length" @click="collapseAll">收起全部</el-button>
                <el-button size="small" :disabled="!treeData.length" :loading="refreshLoading" @click="onRefreshState">
                  刷新状态
                </el-button>
              </div>
            </div>
          </template>
          <el-tree
            v-if="treeData.length"
            ref="treeRef"
            :data="treeData"
            :default-expanded-keys="defaultExpandedKeys"
            node-key="id"
            :props="{ label: 'label', children: 'children' }"
            :check-on-click-node="false"
            class="catalog-tree scope-tree"
            @node-click="onNodeClick"
          >
            <template #default="{ data }">
              <span class="scope-tree-row">
                <el-checkbox
                  :model-value="isDirFull(data.id)"
                  :indeterminate="isDirHalf(data.id)"
                  @click.stop
                  @change="(v) => onDirToggle(data.id, v)"
                />
                <span class="node-label">{{ data.label }}</span>
              </span>
            </template>
          </el-tree>
          <div v-else-if="!treeLoading" class="empty-tip">暂无编组数据，请先在「目录编组」中维护本机树。</div>
        </el-card>
      </section>

      <section class="catalog-right">
        <template v-if="selectedNode">
          <el-card shadow="hover" class="detail-card">
            <template #header>
              <span>节点详情</span>
            </template>
            <el-descriptions :column="2" border size="small">
              <el-descriptions-item label="名称">{{ selectedNode.name }}</el-descriptions-item>
              <el-descriptions-item label="类型">{{ nodeTypeLabel(selectedNode.nodeType) }}</el-descriptions-item>
              <el-descriptions-item label="编组国标" :span="2">{{ selectedNode.gbDeviceId || '—' }}</el-descriptions-item>
            </el-descriptions>
          </el-card>

          <el-card shadow="hover" class="cameras-card">
            <template #header>
              <span>挂载摄像头（{{ nodeCameraTotal }}）</span>
            </template>
            <el-table
              v-loading="nodeCamerasLoading"
              :data="pagedNodeCameras"
              height="320"
              size="small"
            >
              <el-table-column label="推送" width="64" fixed>
                <template #default="{ row }">
                  <span class="push-cell-check" @click.stop>
                    <el-checkbox
                      :model-value="isCameraPushed(row)"
                      @change="(v) => onCameraPushToggle(row, v)"
                    />
                  </span>
                </template>
              </el-table-column>
              <el-table-column prop="name" label="名称" min-width="110" />
              <el-table-column prop="catalogGbDeviceId" label="编组国标" min-width="130" />
              <el-table-column prop="deviceGbId" label="注册国标" min-width="130" />
              <el-table-column prop="platformName" label="平台" width="90" />
              <el-table-column label="摄像" width="72">
                <template #default="{ row }">
                  <el-tag :type="row.online ? 'success' : 'info'" size="small">
                    {{ row.online ? '在线' : '离线' }}
                  </el-tag>
                </template>
              </el-table-column>
              <el-table-column label="操作" width="120" fixed="right">
                <template #default="{ row }">
                  <el-button size="small" type="primary" text @click.stop="onLivePreview(row)">预览</el-button>
                  <el-button size="small" text @click.stop="goReplay(row)">回看</el-button>
                </template>
              </el-table-column>
            </el-table>
            <div v-if="nodeCameraTotal > pageSize" class="table-footer">
              <el-pagination
                v-model:current-page="cameraPage"
                v-model:page-size="pageSize"
                :page-sizes="[10, 20, 50]"
                layout="total, sizes, prev, pager, next"
                :total="nodeCameraTotal"
                small
                @size-change="onNodeCameraPageChange"
                @current-change="onNodeCameraPageChange"
              />
            </div>
            <div v-else-if="nodeCameraTotal === 0" class="empty-tip">当前节点下暂无挂载。</div>
          </el-card>
        </template>
        <template v-else>
          <el-card shadow="hover">
            <div class="empty-tip">请在左侧选择目录节点。</div>
          </el-card>
        </template>
      </section>
    </div>

    <div class="footer-actions">
      <el-button
        type="primary"
        plain
        :disabled="!treeData.length"
        :loading="previewPrepLoading"
        @click="openPreviewDialog"
      >
        推送预览
      </el-button>
      <el-button type="primary" :loading="saveLoading" :disabled="!platform || !treeData.length" @click="onSaveScope">
        保存范围
      </el-button>
      <el-button
        type="success"
        :loading="notifyLoading"
        :disabled="!platform || !platform.enabled"
        @click="onCatalogNotify"
      >
        目录上报
      </el-button>
    </div>

    <el-dialog v-model="previewVisible" title="推送预览（将上报的国标）" width="min(920px, 96vw)" destroy-on-close>
      <p v-if="!previewDirRows.length && !previewCamRows.length" class="empty-tip">当前无推送内容。</p>
      <template v-else>
        <p class="preview-summary">
          目录根 {{ payloadSnapshot.catalogGroupNodeIds.length }} 个 · 目录项 {{ previewDirRows.length }} 个 · 通道
          {{ previewCamRows.length }} 个 · 排除 {{ excludedCameraIds.length }} 个
        </p>
        <h4 class="preview-block-title">目录</h4>
        <el-table :data="previewDirRows" size="small" max-height="240" border>
          <el-table-column prop="name" label="名称" min-width="120" />
          <el-table-column prop="gbDeviceId" label="编组国标（上报 DeviceID）" min-width="180" />
        </el-table>
        <h4 class="preview-block-title">通道</h4>
        <el-table :data="previewCamRows" size="small" max-height="280" border>
          <el-table-column prop="catalogGbDeviceId" label="编组通道国标" min-width="160" />
          <el-table-column prop="deviceGbId" label="注册国标" min-width="140" />
          <el-table-column prop="name" label="名称" min-width="100" />
          <el-table-column label="操作" width="120" fixed="right">
            <template #default="{ row }">
              <el-button size="small" type="primary" text @click.stop="onLivePreview(row)">预览</el-button>
              <el-button size="small" text @click.stop="goReplay(row)">回看</el-button>
            </template>
          </el-table-column>
        </el-table>
      </template>
      <template #footer>
        <el-button type="primary" @click="previewVisible = false">关闭</el-button>
      </template>
    </el-dialog>

    <el-dialog
      v-model="livePreviewVisible"
      :title="livePreviewTitle"
      width="800px"
      top="8vh"
      destroy-on-close
      @closed="onLivePreviewDialogClose"
    >
      <div v-if="livePreviewCamera">
        <div v-if="livePreviewState === 'error'" class="preview-err">{{ livePreviewError }}</div>
        <div ref="livePlayerContainer" class="player-wrap" />
        <PtzPanel
          :camera-id="String(livePreviewRowId || '')"
          :platform-gb-id="String(livePreviewCamera.platformGbId || '')"
          :platform-db-id="livePreviewCamera.platformDbId"
        />
      </div>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, watch, nextTick, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import PtzPanel from '@/components/PtzPanel.vue'
import { api } from '@/api/client'
import { createJessibucaPlayer, destroyJessibucaPlayer } from '@/composables/useJessibuca.js'

const route = useRoute()
const router = useRouter()
const upstreamId = computed(() => String(route.query.upstreamId || '').trim())

const platformLoading = ref(true)
const platform = ref(null)
const treeLoading = ref(false)
const refreshLoading = ref(false)
const previewPrepLoading = ref(false)
const treeRef = ref(null)
const rawGroupItems = ref([])
const mountRows = ref([])

const selectedNode = ref(null)
const nodeCameraList = ref([])
const nodeCameraTotal = ref(0)
const nodeCamerasLoading = ref(false)
const cameraPage = ref(1)
const pageSize = ref(20)

/**
 * 三态勾选模型（替代原 el-tree 原生勾选 + excludedCameraIds）
 * - fullCheckedDirs：用户直接勾选的目录 id（覆盖整个子树推送）
 * - partialCameraIds：用户在右侧单独勾选的摄像头 id（其所在目录未被 full 覆盖）
 * 两个集合始终保持"最小化、无冗余"：full 覆盖的后代不再单列，其子树摄像头不出现在 partial 里。
 */
const fullCheckedDirs = ref(new Set())
const partialCameraIds = ref(new Set())
const checkTick = ref(0)
const saveLoading = ref(false)
const notifyLoading = ref(false)
const previewVisible = ref(false)

const livePreviewVisible = ref(false)
const livePreviewCamera = ref(null)
const livePreviewRowId = ref('')
const livePreviewState = ref('idle')
const livePreviewError = ref('')
const livePlayerContainer = ref(null)
let livePlayerHandle = null

const livePreviewTitle = computed(() => {
  if (!livePreviewCamera.value) return '实时预览'
  return `实时预览 - ${livePreviewCamera.value.name}`
})

let savedScopeSignature = ''

function mapApiNode(n) {
  const id = String(n.id)
  const children = (n.children || []).map(mapApiNode)
  return {
    id,
    parentId: n.parentId == null ? null : String(n.parentId),
    label: `${n.name}（${n.gbDeviceId || ''}）`,
    name: n.name,
    gbDeviceId: n.gbDeviceId,
    nodeType: n.nodeType,
    children,
  }
}

const treeData = computed(() => (rawGroupItems.value || []).map(mapApiNode))
const defaultExpandedKeys = computed(() => (treeData.value[0]?.id ? [treeData.value[0].id] : []))

function buildDirChildrenMap(items, parentKey = 'ROOT', acc = new Map()) {
  for (const n of items || []) {
    const id = String(n.id)
    if (!acc.has(parentKey)) acc.set(parentKey, [])
    acc.get(parentKey).push(id)
    if (n.children?.length) buildDirChildrenMap(n.children, id, acc)
  }
  return acc
}

const dirChildrenMap = computed(() => buildDirChildrenMap(rawGroupItems.value))

function buildPlainDirParentMap(items, parentId = null, out = {}) {
  for (const n of items || []) {
    const id = String(n.id)
    out[id] = parentId == null ? null : String(parentId)
    if (n.children?.length) buildPlainDirParentMap(n.children, n.id, out)
  }
  return out
}

const plainDirParentMap = computed(() => buildPlainDirParentMap(rawGroupItems.value))

/** 目录 id -> 名称、编组国标（预览用） */
function buildDirMetaMap(items, acc = new Map()) {
  for (const n of items || []) {
    acc.set(String(n.id), { name: n.name, gbDeviceId: n.gbDeviceId || '' })
    if (n.children?.length) buildDirMetaMap(n.children, acc)
  }
  return acc
}

const dirMetaMap = computed(() => buildDirMetaMap(rawGroupItems.value))

function collectDescendantDirIds(rootIdStr, map) {
  const rid = String(rootIdStr)
  const out = new Set()
  function dfs(id) {
    out.add(String(id))
    for (const ch of map.get(id) || []) dfs(ch)
  }
  dfs(rid)
  return out
}

function collectScopeDirSet(rootNumericIds, map) {
  const set = new Set()
  for (const r of rootNumericIds || []) {
    collectDescendantDirIds(String(r), map).forEach((x) => set.add(x))
  }
  return set
}

function minimizeScopeRoots(checkedIds, pmap) {
  const unique = [...new Set((checkedIds || []).map((x) => String(x)))]
  const set = new Set(unique)
  const out = []
  for (const sid of unique) {
    let p = pmap[sid]
    let covered = false
    while (p != null && p !== '') {
      if (set.has(String(p))) {
        covered = true
        break
      }
      p = pmap[p]
    }
    if (!covered) out.push(sid)
  }
  return out
}

/** 统一为十进制字符串用于集合比较 */
function camIdStr(v) {
  if (v == null || v === '') return ''
  return String(v).trim()
}

/** 沿 parent 链判断 dirId 是否被 fullSet 中任一祖先覆盖（不含自身） */
function isAncestorCovered(dirId, fullSet) {
  const pmap = plainDirParentMap.value
  let p = pmap[String(dirId)]
  const seen = new Set()
  while (p != null && p !== '' && !seen.has(p)) {
    seen.add(p)
    if (fullSet.has(String(p))) return true
    p = pmap[String(p)]
  }
  return false
}

/** dirId 自身或其某个祖先在 fullCheckedDirs 中 → 全选 */
function isDirFull(dirId) {
  checkTick.value
  const sid = String(dirId)
  if (fullCheckedDirs.value.has(sid)) return true
  return isAncestorCovered(sid, fullCheckedDirs.value)
}

/** 收集以 dirId 为根的子树内所有挂载摄像头 id（number） */
function collectCamIdsUnder(dirId) {
  const subDirs = collectDescendantDirIds(String(dirId), dirChildrenMap.value)
  const out = []
  for (const m of mountRows.value || []) {
    const gid = String(m.groupNodeId ?? '').trim()
    if (subDirs.has(gid)) {
      const cid = Number(m.cameraId)
      if (Number.isFinite(cid) && cid > 0) out.push(cid)
    }
  }
  return out
}

/** dirId 非 full，且子树存在 partial 摄像头 或 子孙目录存在 full → 半选 */
function isDirHalf(dirId) {
  checkTick.value
  const sid = String(dirId)
  if (isDirFull(sid)) return false
  const subDirs = collectDescendantDirIds(sid, dirChildrenMap.value)
  // 子孙目录（含自身）是否存在 full
  for (const fd of fullCheckedDirs.value) {
    if (subDirs.has(String(fd))) return true
  }
  // 子树内是否存在 partial 摄像头
  if (partialCameraIds.value.size > 0) {
    for (const m of mountRows.value || []) {
      const gid = String(m.groupNodeId ?? '').trim()
      if (!subDirs.has(gid)) continue
      const cid = Number(m.cameraId)
      if (partialCameraIds.value.has(cid)) return true
    }
  }
  return false
}

/** 右侧摄像头勾选状态：被 full 祖先覆盖 或 在 partial 集合内 */
function isCameraPushed(row) {
  checkTick.value
  const gid = String(row.groupNodeId ?? row.catalogGroupNodeId ?? selectedNode.value?.id ?? '').trim()
  if (gid) {
    const sid = gid
    if (fullCheckedDirs.value.has(sid) || isAncestorCovered(sid, fullCheckedDirs.value)) return true
  }
  const cid = Number(row.cameraId ?? row.id)
  if (Number.isFinite(cid) && partialCameraIds.value.has(cid)) return true
  return false
}

/**
 * 将某被祖先覆盖的目录 dirId "降级"：
 * 找到覆盖它的最近 full 祖先 A，把 A 从 full 移除，
 * 沿 A → dirId 的路径，对每一步节点的"直接兄弟"重新加回 full（保持它们子树被覆盖），
 * 并把 dirId 路径节点的挂载摄像头原样加入 partial（保持其下摄像头仍是勾选）。
 * 最终调用方会在 dirId 上做进一步操作（例如排除某摄像头）。
 */
function expandAncestorFullCheck(dirId) {
  const pmap = plainDirParentMap.value
  const sid = String(dirId)
  // 找覆盖祖先
  let anc = pmap[sid]
  const chain = [sid]
  while (anc != null && anc !== '') {
    if (fullCheckedDirs.value.has(String(anc))) break
    chain.push(String(anc))
    anc = pmap[String(anc)]
  }
  if (anc == null || anc === '' || !fullCheckedDirs.value.has(String(anc))) {
    return false // 实际上未被覆盖
  }
  const ancStr = String(anc)
  // 从 full 移除祖先 A
  fullCheckedDirs.value.delete(ancStr)
  // 沿 A 的直接子向下到 dirId：每一步，把"该节点的兄弟"加入 full；把"该节点自身"保持链条继续
  // chain 目前是 [dirId, ..., ancParent]（不含 anc 本身），反向为 anc → ... → dirId 的路径
  // 需要从 anc 的直接子开始向下，沿 chain 反向展开
  const pathDown = [] // 从 anc 的直接子一路到 dirId
  let cur = sid
  while (cur !== ancStr) {
    pathDown.push(cur)
    cur = pmap[cur]
    if (cur == null) break
  }
  pathDown.reverse() // 现在从靠近 anc 的子 → dirId
  // 添加 anc 直接子中不在路径上的兄弟为 full
  let prev = ancStr
  for (const step of pathDown) {
    const siblings = dirChildrenMap.value.get(prev) || []
    for (const sib of siblings) {
      if (String(sib) !== String(step)) {
        fullCheckedDirs.value.add(String(sib))
      }
    }
    prev = step
  }
  // dirId 本身不再 full（调用方随后决定其状态），但其子孙目录要全部变 full 以保留"整子树被推送"
  // 实际上：如果 dirId 后续仅排除一个摄像头，其子孙目录依然要推 —— 把 dirId 的直接子全部加入 full
  for (const child of dirChildrenMap.value.get(sid) || []) {
    fullCheckedDirs.value.add(String(child))
  }
  // 把 dirId 自身挂载的摄像头加入 partial（保留勾选状态；调用方会随后删除目标摄像头）
  for (const m of mountRows.value || []) {
    if (String(m.groupNodeId ?? '').trim() === sid) {
      const cid = Number(m.cameraId)
      if (Number.isFinite(cid) && cid > 0) partialCameraIds.value.add(cid)
    }
  }
  return true
}

/** 目录勾选切换 */
function onDirToggle(dirId, checked) {
  const sid = String(dirId)
  if (checked) {
    // 加入 full，并清理被覆盖的冗余
    // 若已被祖先覆盖，无需再加
    if (!isAncestorCovered(sid, fullCheckedDirs.value)) {
      fullCheckedDirs.value.add(sid)
    }
    // 清理后代 full：sid 子树内的 full 成员都冗余
    const sub = collectDescendantDirIds(sid, dirChildrenMap.value)
    for (const fd of [...fullCheckedDirs.value]) {
      if (fd !== sid && sub.has(String(fd))) fullCheckedDirs.value.delete(fd)
    }
    // 清理子树内的 partial（被 full 覆盖后冗余）
    for (const cid of [...partialCameraIds.value]) {
      const m = (mountRows.value || []).find((x) => Number(x.cameraId) === cid)
      if (m && sub.has(String(m.groupNodeId ?? '').trim())) partialCameraIds.value.delete(cid)
    }
  } else {
    // 取消
    if (fullCheckedDirs.value.has(sid)) {
      fullCheckedDirs.value.delete(sid)
    } else if (isAncestorCovered(sid, fullCheckedDirs.value)) {
      // 被祖先覆盖：先展开到 dirId 的父级，再从 dirId 开始取消（即 dirId 的直接子不再 full）
      expandAncestorFullCheck(sid)
      // 展开后，dirId 的"直接子"已经全部 full；需要把它们再次移除（因为 dirId 本身被取消 = 整个 dirId 子树不勾）
      // 实际做法：移除 dirId 子树内所有 full / partial
      const sub = collectDescendantDirIds(sid, dirChildrenMap.value)
      for (const fd of [...fullCheckedDirs.value]) {
        if (sub.has(String(fd))) fullCheckedDirs.value.delete(fd)
      }
      for (const cid of [...partialCameraIds.value]) {
        const m = (mountRows.value || []).find((x) => Number(x.cameraId) === cid)
        if (m && sub.has(String(m.groupNodeId ?? '').trim())) partialCameraIds.value.delete(cid)
      }
    }
    // 其他情况：本就未勾，无操作
  }
  refreshPayloadSnapshot()
  checkTick.value++
}

function onCameraPushToggle(row, checked) {
  const cid = Number(row.cameraId ?? row.id)
  if (!Number.isFinite(cid) || cid <= 0) return
  const gid = String(row.groupNodeId ?? row.catalogGroupNodeId ?? selectedNode.value?.id ?? '').trim()
  if (checked) {
    // 若已被 full 覆盖，无需操作
    if (gid && (fullCheckedDirs.value.has(gid) || isAncestorCovered(gid, fullCheckedDirs.value))) {
      // already pushed
    } else {
      partialCameraIds.value.add(cid)
    }
  } else {
    // 取消勾选
    if (partialCameraIds.value.has(cid)) {
      partialCameraIds.value.delete(cid)
    } else if (gid && (fullCheckedDirs.value.has(gid) || isAncestorCovered(gid, fullCheckedDirs.value))) {
      // 被 full 覆盖：降级为 partial，然后把目标摄像头从 partial 移除
      if (fullCheckedDirs.value.has(gid)) {
        // gid 本身 full：直接改为把 gid 子树摄像头（除目标）加入 partial
        fullCheckedDirs.value.delete(gid)
        // 保留 gid 的直接子目录为 full（子树不变）
        for (const child of dirChildrenMap.value.get(gid) || []) {
          fullCheckedDirs.value.add(String(child))
        }
        // gid 自身挂载的摄像头 → partial（除目标）
        for (const m of mountRows.value || []) {
          if (String(m.groupNodeId ?? '').trim() === gid) {
            const n = Number(m.cameraId)
            if (Number.isFinite(n) && n > 0 && n !== cid) partialCameraIds.value.add(n)
          }
        }
      } else {
        expandAncestorFullCheck(gid)
        // expandAncestorFullCheck 已把 gid 挂载摄像头加入 partial；现删除目标
        partialCameraIds.value.delete(cid)
      }
    }
  }
  refreshPayloadSnapshot()
  checkTick.value++
}

/**
 * computePayload：从两个集合生成后端 API 载荷
 * - fullCheckedDirs 直接作为 scope 根候选
 * - partialCameraIds 的挂载目录（若未被 full 祖先覆盖）作为 scope 根候选（半选根）
 * - 去祖先冗余后作为 roots
 * - 以 roots 子树为范围，扫描挂载表，对未被 full 覆盖且未在 partial 的摄像头 → excludedCameraIds
 *
 * 后端 loadSubtreeRows 会自动带出 roots 的祖先链作为目录项，所以半选目录无需提升到顶层祖先。
 */
function computePayload() {
  const pmap = plainDirParentMap.value
  const candidates = new Set()
  for (const fd of fullCheckedDirs.value) candidates.add(String(fd))
  const dirOfCam = new Map()
  for (const m of mountRows.value || []) {
    const cid = Number(m.cameraId)
    if (Number.isFinite(cid)) dirOfCam.set(cid, String(m.groupNodeId ?? '').trim())
  }
  for (const cid of partialCameraIds.value) {
    const gid = dirOfCam.get(cid)
    if (!gid) continue
    if (fullCheckedDirs.value.has(gid)) continue
    if (isAncestorCovered(gid, fullCheckedDirs.value)) continue
    candidates.add(gid)
  }
  const mini = minimizeScopeRoots([...candidates], pmap)
  const roots = mini.map((x) => Number(x)).filter((n) => Number.isFinite(n) && n > 0)

  const scope = collectScopeDirSet(roots, dirChildrenMap.value)
  const excluded = []
  const seenEx = new Set()
  for (const m of mountRows.value || []) {
    const gid = String(m.groupNodeId ?? '').trim()
    if (!scope.has(gid)) continue
    const cid = Number(m.cameraId)
    if (!Number.isFinite(cid) || cid <= 0) continue
    const pushed =
      fullCheckedDirs.value.has(gid) ||
      isAncestorCovered(gid, fullCheckedDirs.value) ||
      partialCameraIds.value.has(cid)
    if (!pushed && !seenEx.has(cid)) {
      seenEx.add(cid)
      excluded.push(cid)
    }
  }
  excluded.sort((a, b) => a - b)
  return { catalogGroupNodeIds: roots, excludedCameraIds: excluded }
}

const payloadSnapshot = ref({ catalogGroupNodeIds: [], excludedCameraIds: [] })
function refreshPayloadSnapshot() {
  payloadSnapshot.value = computePayload()
}

const scopeDirSet = computed(() => collectScopeDirSet(payloadSnapshot.value.catalogGroupNodeIds, dirChildrenMap.value))

const isSelectedDirInScope = computed(() => {
  if (!selectedNode.value?.id) return false
  return scopeDirSet.value.has(String(selectedNode.value.id))
})

function nodeTypeLabel(t) {
  const n = Number(t)
  if (n === 3) return '业务分组（215）'
  if (n === 1) return '虚拟组织/目录（216）'
  if (n === 2) return '行政区域'
  if (n === 0) return '通道占位'
  return String(t ?? '—')
}

const pagedNodeCameras = computed(() => nodeCameraList.value || [])

function onNodeClick(data) {
  selectedNode.value = data
  cameraPage.value = 1
  loadNodeCameras()
}

function onNodeCameraPageChange() {
  loadNodeCameras()
}

async function loadNodeCameras() {
  const node = selectedNode.value
  if (!node?.id) {
    nodeCameraList.value = []
    nodeCameraTotal.value = 0
    return
  }
  nodeCamerasLoading.value = true
  try {
    const res = await api.getCatalogGroupNodeCameras(node.id, {
      page: String(cameraPage.value),
      pageSize: String(pageSize.value),
    })
    if (res.code === 0 && res.data?.items) {
      const selGid = String(selectedNode.value?.id ?? '').trim()
      nodeCameraList.value = res.data.items.map((r) => ({
        id: r.cameraId,
        cameraId: r.cameraId,
        groupNodeId: r.groupNodeId ?? selGid,
        catalogGbDeviceId: r.catalogGbDeviceId,
        deviceGbId: r.deviceGbId,
        name: r.name,
        platformName: r.platformName,
        platformGbId: r.platformGbId,
        platformDbId: r.platformDbId,
        online: r.online === true || r.online === 'true',
      }))
      nodeCameraTotal.value = Number(res.data.total || nodeCameraList.value.length)
    } else {
      nodeCameraList.value = []
      nodeCameraTotal.value = 0
    }
  } catch {
    nodeCameraList.value = []
    nodeCameraTotal.value = 0
  } finally {
    nodeCamerasLoading.value = false
  }
}

function signaturePayload(roots, excluded) {
  const r = [...new Set((roots || []).map((x) => Number(x)))].sort((a, b) => a - b)
  const e = [...new Set((excluded || []).map((x) => Number(x)))].sort((a, b) => a - b)
  return JSON.stringify({ r, e })
}

const isDirty = computed(() => {
  const cur = payloadSnapshot.value
  return signaturePayload(cur.catalogGroupNodeIds, cur.excludedCameraIds) !== savedScopeSignature
})

const previewDirRows = computed(() => {
  const p = payloadSnapshot.value
  const sdir = collectScopeDirSet(p.catalogGroupNodeIds, dirChildrenMap.value)
  const meta = dirMetaMap.value
  const rows = []
  for (const id of sdir) {
    const m = meta.get(id)
    if (m) rows.push({ name: m.name, gbDeviceId: m.gbDeviceId })
  }
  rows.sort((a, b) => (a.gbDeviceId || '').localeCompare(b.gbDeviceId || ''))
  return rows
})

const previewCamRows = computed(() => {
  const p = payloadSnapshot.value
  const ex = new Set((p.excludedCameraIds || []).map((x) => camIdStr(x)).filter(Boolean))
  const sdir = collectScopeDirSet(p.catalogGroupNodeIds, dirChildrenMap.value)
  const rows = []
  for (const m of mountRows.value || []) {
    const gid = String(m.groupNodeId ?? '').trim()
    if (!sdir.has(gid)) continue
    if (ex.has(camIdStr(m.cameraId))) continue
    rows.push({
      id: m.cameraId,
      cameraId: m.cameraId,
      catalogGbDeviceId: m.catalogGbDeviceId || '—',
      deviceGbId: m.deviceGbId || '—',
      name: m.name || '—',
      platformGbId: m.platformGbId,
      platformDbId: m.platformDbId,
    })
  }
  rows.sort((a, b) => String(a.catalogGbDeviceId).localeCompare(String(b.catalogGbDeviceId)))
  return rows
})

async function openPreviewDialog() {
  previewPrepLoading.value = true
  try {
    refreshPayloadSnapshot()
    previewVisible.value = true
  } finally {
    previewPrepLoading.value = false
  }
}

function destroyLivePlayer() {
  if (livePlayerHandle) {
    destroyJessibucaPlayer(livePlayerHandle)
    livePlayerHandle = null
  }
}

async function onLivePreview(row) {
  livePreviewCamera.value = row
  livePreviewRowId.value = String(row.id ?? row.cameraId ?? '')
  livePreviewVisible.value = true
  livePreviewState.value = 'loading'
  livePreviewError.value = ''
  try {
    const res = await api.startPreview(
      livePreviewRowId.value,
      row.platformGbId,
      row.platformDbId,
      row.deviceGbId,
    )
    if (res.code !== 0) {
      livePreviewState.value = 'error'
      livePreviewError.value = res.message || '发起点播失败'
      ElMessage.error(livePreviewError.value)
      return
    }
    const flvUrl = res.data?.flvUrl
    await nextTick()
    await nextTick()
    destroyLivePlayer()
    livePlayerHandle = await createJessibucaPlayer(livePlayerContainer.value, flvUrl, {
      onError: (err) => {
        livePreviewState.value = 'error'
        livePreviewError.value = String(err?.message || err)
      },
    })
    livePreviewState.value = 'playing'
  } catch (e) {
    livePreviewState.value = 'error'
    livePreviewError.value = e?.message || '连接失败'
    ElMessage.error(livePreviewError.value)
  }
}

function onLivePreviewDialogClose() {
  destroyLivePlayer()
  livePreviewState.value = 'idle'
  livePreviewError.value = ''
  livePreviewCamera.value = null
  livePreviewRowId.value = ''
}

function goReplay(row) {
  router.push({
    path: '/live/replay',
    query: {
      cameraId: row.id ?? row.cameraId,
      cameraName: row.name,
      platformGbId: String(row.platformGbId || '').trim(),
      platformDbId: row.platformDbId != null && row.platformDbId !== '' ? String(row.platformDbId) : '',
    },
  })
}

function goBack() {
  if (window.history.length > 1) router.back()
  else router.push('/platforms')
}

function expandAll() {
  const nodes = treeRef.value?.store?.nodesMap
  if (!nodes) return
  for (const k of Object.keys(nodes)) nodes[k].expanded = true
}

function collapseAll() {
  const nodes = treeRef.value?.store?.nodesMap
  if (!nodes) return
  for (const k of Object.keys(nodes)) nodes[k].expanded = false
}

async function loadPlatform() {
  platformLoading.value = true
  platform.value = null
  try {
    const id = upstreamId.value
    if (!id) {
      ElMessage.warning('缺少上级平台参数')
      router.replace('/platforms')
      return
    }
    const res = await api.getUpstreamPlatform(id)
    const row = res?.data || null
    if (!row) {
      ElMessage.error('未找到该上级平台')
      router.replace('/platforms')
      return
    }
    platform.value = {
      id: row.id,
      name: row.name ?? '',
      gbId: row.gbId ?? '',
      sipIp: row.sipIp ?? '',
      sipPort: row.sipPort ?? 5060,
      transport: row.transport ?? 'udp',
      enabled: row.enabled !== false,
      online: !!row.online,
      catalogGroupNodeIds: Array.isArray(row.catalogGroupNodeIds) ? row.catalogGroupNodeIds : [],
      excludedCameraIds: Array.isArray(row.excludedCameraIds) ? row.excludedCameraIds : [],
    }
  } catch (e) {
    ElMessage.error(e?.message || '加载上级平台失败')
    router.replace('/platforms')
  } finally {
    platformLoading.value = false
  }
}

async function loadTreeAndMounts() {
  treeLoading.value = true
  try {
    const [nodesRes, mountRes] = await Promise.all([
      api.getCatalogGroupNodes({ nested: '1' }),
      api.getCatalogGroupCameraMounts(),
    ])
    if (nodesRes.code === 0 && nodesRes.data?.items) rawGroupItems.value = nodesRes.data.items
    else rawGroupItems.value = []
    if (mountRes.code === 0 && mountRes.data?.items) mountRows.value = mountRes.data.items
    else mountRows.value = []
  } catch (e) {
    ElMessage.error(e?.message || '加载编组数据失败')
    rawGroupItems.value = []
    mountRows.value = []
  } finally {
    treeLoading.value = false
  }
}

/**
 * 从服务端 scope + excluded 反推两个集合：
 * - 对每个 root：收集子树内挂载摄像头
 *   - 若全部不在 excluded → root 加入 fullCheckedDirs
 *   - 若部分在 excluded → root 不进 full；把未 excluded 的摄像头加入 partialCameraIds
 *   - 若全部都在 excluded → 丢弃（相当于没勾）
 * 完成后再做一次最小化：fullCheckedDirs 间去祖先冗余
 */
async function applyDefaultFromServer() {
  const plat = platform.value
  if (!plat) return
  const dmap = dirChildrenMap.value
  const idInTree = new Set()
  for (const k of dmap.keys()) {
    if (k !== 'ROOT') idInTree.add(k)
  }
  for (const ids of dmap.values()) ids.forEach((x) => idInTree.add(x))

  const roots = [
    ...new Set((plat.catalogGroupNodeIds || []).map((x) => Number(x)).filter((n) => idInTree.has(String(n)))),
  ]
  const excludedSet = new Set(
    (plat.excludedCameraIds || []).map((x) => Number(x)).filter((n) => Number.isFinite(n) && n > 0),
  )

  const newFull = new Set()
  const newPartial = new Set()
  for (const r of roots) {
    const cams = collectCamIdsUnder(r)
    if (cams.length === 0) {
      // 子树无摄像头挂载：保留 full 以使目录节点本身被上报
      newFull.add(String(r))
      continue
    }
    const inExcluded = cams.filter((c) => excludedSet.has(c))
    const notExcluded = cams.filter((c) => !excludedSet.has(c))
    if (inExcluded.length === 0) {
      newFull.add(String(r))
    } else if (notExcluded.length === 0) {
      // 全被排除：相当于未勾
    } else {
      for (const c of notExcluded) newPartial.add(c)
    }
  }
  // 去除 full 间的祖先冗余
  const miniFull = minimizeScopeRoots([...newFull], plainDirParentMap.value)
  fullCheckedDirs.value = new Set(miniFull)
  partialCameraIds.value = newPartial

  refreshPayloadSnapshot()
  const cur = payloadSnapshot.value
  savedScopeSignature = signaturePayload(cur.catalogGroupNodeIds, cur.excludedCameraIds)
  checkTick.value++
}

async function onRefreshState() {
  const savedFull = new Set(fullCheckedDirs.value)
  const savedPartial = new Set(partialCameraIds.value)
  refreshLoading.value = true
  try {
    await loadTreeAndMounts()
    await nextTick()
    // 挂载/目录树变化后，过滤掉已不存在的目录和摄像头
    const dmap = dirChildrenMap.value
    const validDirs = new Set()
    for (const ids of dmap.values()) ids.forEach((x) => validDirs.add(String(x)))
    fullCheckedDirs.value = new Set([...savedFull].filter((d) => validDirs.has(String(d))))
    const validCamIds = new Set(
      (mountRows.value || []).map((m) => Number(m.cameraId)).filter((n) => Number.isFinite(n)),
    )
    partialCameraIds.value = new Set([...savedPartial].filter((c) => validCamIds.has(c)))
    refreshPayloadSnapshot()
    checkTick.value++
    if (selectedNode.value) await loadNodeCameras()
    ElMessage.success('已刷新')
  } finally {
    refreshLoading.value = false
  }
}

async function onSaveScope() {
  if (!platform.value) return
  saveLoading.value = true
  try {
    const cur = computePayload()
    const roots = cur.catalogGroupNodeIds
    const excludedToSave = cur.excludedCameraIds
    const res = await api.putPlatformCatalogScope(platform.value.id, {
      catalogGroupNodeIds: roots,
      excludedCameraIds: excludedToSave,
    })
    if (res.code !== 0) {
      ElMessage.error(res.message || '保存失败')
      return
    }
    savedScopeSignature = signaturePayload(roots, excludedToSave)
    platform.value.catalogGroupNodeIds = roots
    platform.value.excludedCameraIds = excludedToSave
    refreshPayloadSnapshot()
    checkTick.value++
    ElMessage.success('推送范围已保存')
  } catch (e) {
    ElMessage.error(e?.message || '保存失败')
  } finally {
    saveLoading.value = false
  }
}

async function doNotify() {
  if (!platform.value) return
  notifyLoading.value = true
  try {
    await api.postUpstreamCatalogNotify(platform.value.id)
    ElMessage.success('目录上报已入队，将由信令线程发送')
  } catch (e) {
    ElMessage.error('上报失败：' + (e.message || '网络错误'))
  } finally {
    notifyLoading.value = false
  }
}

async function onCatalogNotify() {
  if (!platform.value) return
  if (!platform.value.enabled) {
    ElMessage.warning('请先启用该上级平台')
    return
  }
  if (!isDirty.value) {
    await doNotify()
    return
  }
  try {
    await ElMessageBox.confirm(
      '当前勾选尚未保存；未保存的修改不会写入本次上报所依据的范围。是否先保存再上报？',
      '目录上报',
      {
        confirmButtonText: '保存并上报',
        cancelButtonText: '仅上报',
        distinguishCancelAndClose: true,
        type: 'warning',
      }
    )
    saveLoading.value = true
    try {
      const cur = computePayload()
      const roots = cur.catalogGroupNodeIds
      const excludedToSave = cur.excludedCameraIds
      const res = await api.putPlatformCatalogScope(platform.value.id, {
        catalogGroupNodeIds: roots,
        excludedCameraIds: excludedToSave,
      })
      if (res.code !== 0) {
        ElMessage.error(res.message || '保存失败')
        return
      }
      savedScopeSignature = signaturePayload(roots, excludedToSave)
      platform.value.catalogGroupNodeIds = roots
      platform.value.excludedCameraIds = excludedToSave
      refreshPayloadSnapshot()
      checkTick.value++
    } finally {
      saveLoading.value = false
    }
    await doNotify()
  } catch (e) {
    if (e === 'cancel') await doNotify()
  }
}

onMounted(async () => {
  const qName = route.query.name
  if (qName && typeof qName === 'string') document.title = `推送范围 · ${qName}`
  await loadPlatform()
  await loadTreeAndMounts()
  await nextTick()
  if (platform.value && treeData.value.length) {
    await applyDefaultFromServer()
  }
  refreshPayloadSnapshot()
})

watch(
  () => route.query.upstreamId,
  async (nid, oid) => {
    if (String(nid || '') === String(oid || '')) return
    selectedNode.value = null
    nodeCameraList.value = []
    await loadPlatform()
    await loadTreeAndMounts()
    await nextTick()
    if (platform.value && treeData.value.length) await applyDefaultFromServer()
  }
)
</script>

<style scoped>
.page-header {
  margin-bottom: 16px;
}

.push-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.push-header-left {
  display: flex;
  align-items: center;
  gap: 12px;
}

.back-btn {
  flex-shrink: 0;
}

.push-page-title {
  margin: 0;
  line-height: 32px;
}

.info-card {
  margin-bottom: 12px;
}

.scope-hint {
  font-size: 13px;
  color: var(--gb-text-muted, #6b7280);
  line-height: 1.5;
  margin: 0 0 12px;
}

.catalog-layout {
  display: flex;
  gap: 20px;
  align-items: flex-start;
  margin-bottom: 16px;
}

.catalog-left {
  flex: 0 0 340px;
  min-width: 0;
}

.catalog-right {
  flex: 1 1 400px;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.header-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.tree-actions {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}

.scope-tree {
  max-height: min(520px, 70vh);
  overflow: auto;
}

.scope-tree-row {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  flex: 1;
  min-width: 0;
}

.scope-tree-row .node-label {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.cameras-card .table-footer {
  margin-top: 12px;
  display: flex;
  justify-content: flex-end;
}

.footer-actions {
  display: flex;
  gap: 12px;
  flex-wrap: wrap;
}

.empty-tip {
  color: var(--gb-text-muted, #6b7280);
  font-size: 13px;
  padding: 16px 0;
  text-align: center;
}

.preview-summary {
  font-size: 13px;
  color: var(--gb-text-muted, #6b7280);
  margin: 0 0 12px;
}

.preview-block-title {
  font-size: 14px;
  margin: 12px 0 8px;
  font-weight: 600;
}

.push-cell-check {
  display: inline-flex;
  align-items: center;
}

.player-wrap {
  width: 100%;
  height: 420px;
  position: relative;
  overflow: hidden;
  border: 1px dashed var(--gb-border-strong, #dcdfe6);
  border-radius: 4px;
  background: #000;
  margin-bottom: 12px;
}

.player-wrap :deep(canvas),
.player-wrap :deep(video) {
  display: block;
  vertical-align: top;
}

.preview-err {
  color: #f56c6c;
  margin-bottom: 8px;
  font-size: 13px;
}
</style>

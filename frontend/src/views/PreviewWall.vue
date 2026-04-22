<!--
  @file PreviewWall.vue
  @brief 实时预览：本域 + 下级平台国标目录树，1/4/9 分屏 Jessibuca
-->
<template>
  <div class="page preview-wall">
    <header class="page-header">
      <div>
        <h2 class="page-title">实时预览</h2>
        <p class="page-subtitle">
          左侧按本域与接入平台展示国标目录树；右侧 1/4/9 分屏预览。先点击选中分屏，再双击树中摄像机节点上屏。
        </p>
      </div>
    </header>

    <div class="wall-body">
      <aside class="wall-tree-panel" v-loading="domainLoading">
        <div class="tree-head">
          <span class="tree-title">目录</span>
          <el-button size="small" type="primary" plain @click="refreshTreeRoot">
            刷新
          </el-button>
        </div>
        <el-tree
          :key="treeVersion"
          class="catalog-tree"
          :props="treeProps"
          :load="loadTreeNode"
          lazy
          highlight-current
          node-key="key"
        >
          <template #default="{ data }">
            <span class="tree-node-row" @dblclick.stop="onTreeRowDblClick(data)">
              <el-icon v-if="data.isDomain" class="tree-icon"><Monitor /></el-icon>
              <el-icon v-else-if="data.isPlatform" class="tree-icon"><Link /></el-icon>
              <el-icon v-else-if="data.isDevice" class="tree-icon"><VideoCamera /></el-icon>
              <el-icon v-else class="tree-icon"><Folder /></el-icon>
              <span
                class="tree-label"
                :class="{
                  'tree-device-online': data.isDevice && data.cameraOnline,
                  'tree-device-offline': data.isDevice && !data.cameraOnline,
                }"
              >{{ data.label }}</span>
            </span>
          </template>
        </el-tree>

        <el-collapse v-model="ptzCollapseActive" class="ptz-collapse">
          <el-collapse-item title="云台控制（当前选中分屏）" name="ptz">
            <PtzPanel
              :camera-id="String(ptzSlot.cameraId || '')"
              :platform-gb-id="String(ptzSlot.platformGbId || '')"
              :platform-db-id="ptzSlot.platformDbId"
              :disabled="!ptzSlot.cameraId"
            />
          </el-collapse-item>
        </el-collapse>

        <p class="tree-hint">展开「本域」→ 平台 → 逐级展开国标目录；目录子节点由一次目录接口在本地挂接。双击摄像机在当前选中分屏播放。</p>
      </aside>

      <section class="wall-player-panel">
        <div class="toolbar">
          <span class="toolbar-label">分屏</span>
          <el-radio-group v-model="gridLayout" size="small" @change="onGridLayoutChange">
            <el-radio-button :value="1">1</el-radio-button>
            <el-radio-button :value="4">4</el-radio-button>
            <el-radio-button :value="9">9</el-radio-button>
          </el-radio-group>
          <span class="toolbar-spacer" />
          <span class="slot-hint">当前分屏：{{ selectedSlot + 1 }}</span>
        </div>

        <div class="grid" :class="`grid-${gridLayout}`">
          <div
            v-for="idx in 9"
            v-show="idx <= gridLayout"
            :key="'cell-' + idx"
            class="cell"
            :class="{ 'cell-active': selectedSlot === idx - 1 }"
            @click="selectedSlot = idx - 1"
          >
            <div class="cell-header">
              <span class="cell-title">{{ slots[idx - 1].name || `分屏 ${idx}` }}</span>
              <el-button
                v-if="slots[idx - 1].cameraId"
                type="danger"
                link
                size="small"
                @click.stop="clearSlot(idx - 1)"
              >
                关闭
              </el-button>
            </div>
            <div class="cell-body">
              <div
                :ref="(el) => setJbHost(el, idx - 1)"
                class="jb-host"
              />
              <div v-if="slots[idx - 1].loading" class="cell-overlay">连接中…</div>
              <div v-else-if="slots[idx - 1].error" class="cell-overlay cell-error">
                {{ slots[idx - 1].error }}
              </div>
              <div v-else-if="!slots[idx - 1].cameraId" class="cell-placeholder">
                点击选中本分屏，双击树中摄像机预览
              </div>
              <div v-if="slots[idx - 1].cameraId" class="cell-osd">
                <div>分辨率 {{ slots[idx - 1].osd.resolution }}</div>
                <div>帧率 {{ slots[idx - 1].osd.fps }}</div>
                <div>码率 {{ slots[idx - 1].osd.kbps }}</div>
                <div>编码 {{ slots[idx - 1].osd.codec }}</div>
              </div>
            </div>
          </div>
        </div>
      </section>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onUnmounted, nextTick } from 'vue'
import { ElMessage } from 'element-plus'
import { Folder, Link, Monitor, VideoCamera } from '@element-plus/icons-vue'
import { api } from '@/api/client'
import { createJessibucaPlayer, destroyJessibucaPlayer } from '@/composables/useJessibuca.js'
import { buildCatalogForestGb28181 } from '@/utils/catalogTreeGb28181.js'
import PtzPanel from '@/components/PtzPanel.vue'

const treeProps = { label: 'label', children: 'children', isLeaf: 'leaf' }

const domainLoading = ref(false)
const domainLabel = ref('本域')
const treeVersion = ref(0)

const gridLayout = ref(4)
const selectedSlot = ref(0)

function emptySlot() {
  return {
    cameraId: null,
    name: '',
    platformGbId: null,
    platformDbId: null,
    streamId: null,
    loading: false,
    error: '',
    osd: { resolution: '—', fps: '—', kbps: '—', codec: '—' },
    playerHandle: null,
  }
}

const slots = ref(Array.from({ length: 9 }, () => emptySlot()))

/** 云台折叠面板：默认收起，需要时展开 */
const ptzCollapseActive = ref([])
const ptzSlot = computed(() => slots.value[selectedSlot.value] || emptySlot())
/** @type {HTMLElement[]} */
const jbHostEls = []

function setJbHost(el, i) {
  jbHostEls[i] = el
}

async function loadTreeNode(node, resolve) {
  if (node.level === 0) {
    domainLoading.value = true
    try {
      const res = await api.getLocalGbConfig()
      const d = res?.data || {}
      const domain = d.domain || ''
      const gbId = d.gbId || ''
      domainLabel.value = domain || gbId || '本域'
      const label = domain ? `${domainLabel.value}` : gbId ? `本域 (${gbId})` : '本域'
      resolve([
        {
          key: `domain-${treeVersion.value}`,
          label,
          isDomain: true,
          leaf: false,
        },
      ])
    } catch (e) {
      ElMessage.error(e?.message || '加载本域信息失败')
      resolve([
        {
          key: `domain-${treeVersion.value}`,
          label: '本域',
          isDomain: true,
          leaf: false,
        },
      ])
    } finally {
      domainLoading.value = false
    }
    return
  }

  const d = node.data
  if (d.isDomain) {
    try {
      const res = await api.listDevicePlatforms({ page: 1, pageSize: 500 })
      const items = res?.data?.items || []
      if (!items.length) {
        resolve([])
        return
      }
      resolve(
        items.map((p) => ({
          key: `plat-${p.id}`,
          label: `${p.name || p.gbId}${p.online ? '' : ' (离线)'}`,
          isPlatform: true,
          platformDbId: p.id,
          platformGbId: p.gbId,
          leaf: false,
        })),
      )
    } catch (e) {
      ElMessage.error(e?.message || '加载平台列表失败')
      resolve([])
    }
    return
  }

  if (d.isPlatform) {
    try {
      const res = await api.getCatalogTree(String(d.platformDbId))
      if (res.code !== 0) {
        ElMessage.error(res.message || '加载目录失败')
        resolve([])
        return
      }
      const items = res?.data?.items || []
      const roots = buildCatalogForestGb28181(items, {
        platformDbId: d.platformDbId,
        platformGbId: d.platformGbId,
      })
      resolve(roots)
    } catch (e) {
      ElMessage.error(e?.message || '加载目录失败')
      resolve([])
    }
    return
  }

  // lazy 模式下不会自动把 data.children 挂成 childNodes，每展开一层都要走 load；
  // 森林已在 buildCatalogForestGb28181 里算好，这里把已有子节点交给 resolve 即可。
  if (d.nodeId != null && !d.isDevice && !d.isPlatform && !d.isDomain) {
    const kids = Array.isArray(d.children) ? d.children : []
    resolve(kids)
    return
  }

  resolve([])
}

function refreshTreeRoot() {
  treeVersion.value += 1
}

/**
 * 双击预览：Element Plus Tree 未提供 node-dblclick，需在自定义节点上监听 dblclick。
 */
async function onTreeRowDblClick(data) {
  if (!data?.isDevice || !data.nodeId) return
  await playToSlot(selectedSlot.value, data)
}

async function clearSlot(idx) {
  const s = slots.value[idx]
  if (s.playerHandle) {
    destroyJessibucaPlayer(s.playerHandle)
  }
  // 不调用 stopPreview：由流媒体「无人观看存活」策略回收会话，关分屏仅销毁前端播放器
  slots.value[idx] = emptySlot()
}

async function onGridLayoutChange() {
  const n = gridLayout.value
  for (let i = n; i < 9; i++) {
    await clearSlot(i)
  }
  if (selectedSlot.value >= n) {
    selectedSlot.value = 0
  }
}

async function playToSlot(idx, data) {
  await clearSlot(idx)
  const slot = { ...emptySlot(), loading: true, error: '' }
  slots.value[idx] = slot

  try {
    const res = await api.startPreview(data.nodeId, data.platformGbId, data.platformDbId)
    if (res.code !== 0) {
      throw new Error(res.message || '发起点播失败')
    }
    const flvUrl = res.data?.flvUrl
    if (!flvUrl) throw new Error('未返回播放地址')

    slots.value[idx] = {
      ...slots.value[idx],
      cameraId: data.nodeId,
      name: data.label || data.nodeId,
      platformGbId: data.platformGbId,
      platformDbId: data.platformDbId,
      streamId: res.data?.streamId,
      loading: true,
      error: '',
    }

    await nextTick()
    let container = jbHostEls[idx]
    if (!container) {
      await new Promise((r) => requestAnimationFrame(r))
      await nextTick()
      container = jbHostEls[idx]
    }
    if (!container) throw new Error('播放器容器未就绪')

    const handle = await createJessibucaPlayer(container, flvUrl, {
      onStats: (s) => {
        const cur = slots.value[idx]
        if (!cur?.cameraId) return
        slots.value[idx] = { ...cur, osd: { ...s } }
      },
      onError: (err) => {
        const cur = slots.value[idx]
        slots.value[idx] = {
          ...cur,
          error: String(err?.message || err || '播放错误'),
          loading: false,
        }
      },
    })

    slots.value[idx] = {
      ...slots.value[idx],
      playerHandle: handle,
      loading: false,
    }
    ElMessage.success('已开始预览')
  } catch (e) {
    slots.value[idx] = {
      ...emptySlot(),
      error: e?.message || String(e),
      loading: false,
    }
    ElMessage.error(e?.message || '预览失败')
  }
}

onUnmounted(async () => {
  for (let i = 0; i < 9; i++) {
    await clearSlot(i)
  }
})
</script>

<style scoped>
.page.preview-wall {
  max-width: none;
  width: 100%;
  box-sizing: border-box;
}

.page-header {
  margin-bottom: 12px;
}

.wall-body {
  display: flex;
  gap: 12px;
  align-items: stretch;
  min-height: calc(100vh - 220px);
}

.wall-tree-panel {
  width: 300px;
  flex-shrink: 0;
  border: 1px solid var(--el-border-color-light);
  border-radius: 8px;
  padding: 10px;
  background: var(--el-bg-color);
  display: flex;
  flex-direction: column;
}

.ptz-collapse {
  margin-top: 10px;
  border: 1px solid var(--el-border-color-lighter);
  border-radius: 6px;
  overflow: hidden;
}

.ptz-collapse :deep(.el-collapse-item__header) {
  padding: 0 10px;
  font-size: 13px;
  height: 36px;
  line-height: 36px;
}

.ptz-collapse :deep(.el-collapse-item__content) {
  padding: 0 8px 8px;
}

.ptz-collapse :deep(.ptz-panel) {
  margin-top: 0;
}

.tree-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}

.tree-title {
  font-weight: 600;
}

.catalog-tree {
  flex: 1;
  overflow: auto;
  min-height: 200px;
}

.tree-node-row {
  display: inline-flex;
  align-items: center;
  gap: 6px;
}

.tree-icon {
  flex-shrink: 0;
}

.tree-label.tree-device-online {
  color: var(--el-color-success);
  font-weight: 500;
}

.tree-label.tree-device-offline {
  color: var(--el-text-color-secondary);
}

.tree-hint {
  font-size: 12px;
  color: var(--el-text-color-secondary);
  margin-top: 8px;
  line-height: 1.4;
}

.wall-player-panel {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
}

.toolbar-label {
  font-size: 13px;
  color: var(--el-text-color-regular);
}

.toolbar-spacer {
  flex: 1;
}

.slot-hint {
  font-size: 12px;
  color: var(--el-text-color-secondary);
}

.grid {
  display: grid;
  gap: 8px;
  flex: 1;
  min-height: 360px;
}

.grid-1 {
  grid-template-columns: 1fr;
  grid-template-rows: 1fr;
}

.grid-4 {
  grid-template-columns: 1fr 1fr;
  grid-template-rows: 1fr 1fr;
}

.grid-9 {
  grid-template-columns: 1fr 1fr 1fr;
  grid-template-rows: 1fr 1fr 1fr;
}

.cell {
  border: 1px solid var(--el-border-color);
  border-radius: 6px;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  background: #0f1419;
  min-height: 120px;
}

.cell-active {
  outline: 2px solid var(--el-color-primary);
  outline-offset: -2px;
}

.cell-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 4px 8px;
  background: rgba(0, 0, 0, 0.45);
  color: #e8eaed;
  font-size: 12px;
}

.cell-title {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.cell-body {
  position: relative;
  flex: 1;
  min-height: 100px;
}

.jb-host {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
}

.cell-placeholder {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--el-text-color-placeholder);
  font-size: 12px;
  text-align: center;
  padding: 8px;
  pointer-events: none;
}

.cell-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.55);
  color: #fff;
  font-size: 13px;
  z-index: 2;
}

.cell-error {
  color: #ffb4b4;
  padding: 8px;
  text-align: center;
}

.cell-osd {
  position: absolute;
  left: 6px;
  bottom: 6px;
  z-index: 3;
  font-size: 11px;
  line-height: 1.35;
  color: #e8f4ff;
  text-shadow: 0 0 4px #000, 0 1px 2px #000;
  pointer-events: none;
  max-width: 90%;
}
</style>

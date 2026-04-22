<!--
  文件名: Catalog.vue
  功能: 本机 GB 目录编组；从下级目录导入；下级预览树只读
  依赖: Element Plus、Vue 3、api.catalogGroup*、catalogTreeGb28181.js
-->
<template>
  <div class="page catalog">
    <header class="page-header">
      <div>
        <h2 class="page-title">目录编组</h2>
        <p class="page-subtitle">
          本机编组树与下级同步目录分离；新增节点可选业务分组（国标类型 215）或虚拟组织/目录（216）；编组国标与注册国标双 ID 展示；导入向导会跳过已占用源目录与已挂载摄像头。
        </p>
      </div>
    </header>

    <el-tabs v-model="mainTab" class="catalog-tabs">
      <el-tab-pane label="本机编组" name="group">
        <div class="catalog-layout">
          <section class="catalog-left">
            <el-card v-loading="treeLoading" shadow="hover">
              <template #header>
                <div class="header-row">
                  <span>编组树</span>
                  <el-button type="primary" size="small" :disabled="hasRoot" @click="onAddRoot">
                    新增根节点
                  </el-button>
                </div>
              </template>
              <div v-if="selectedNode" class="tree-toolbar">
                <el-button size="small" @click="onAddChild">新增子节点</el-button>
                <el-button size="small" @click="onEditNode">编辑</el-button>
                <el-button size="small" type="danger" plain @click="onDeleteNode">删除</el-button>
              </div>
              <el-tree
                ref="treeRef"
                :data="treeData"
                node-key="id"
                :props="{ label: 'label', children: 'children' }"
                highlight-current
                default-expand-all
                class="catalog-tree"
                @node-click="onNodeClick"
              />
            </el-card>
          </section>

          <section class="catalog-right">
            <template v-if="selectedNode">
              <el-card shadow="hover" class="detail-card">
                <template #header>
                  <span>节点详情</span>
                  <el-button type="primary" size="small" class="assign-btn" @click="openAssignDialog">
                    分配摄像头
                  </el-button>
                </template>
                <el-descriptions :column="2" border size="small">
                  <el-descriptions-item label="名称">{{ selectedNode.name }}</el-descriptions-item>
                  <el-descriptions-item label="类型">{{ nodeTypeLabel(selectedNode.nodeType) }}</el-descriptions-item>
                  <el-descriptions-item label="编组国标" :span="2">{{ selectedNode.gbDeviceId || '—' }}</el-descriptions-item>
                </el-descriptions>
              </el-card>

              <el-card shadow="hover" class="cameras-card">
                <template #header>
                  <span>挂载摄像头（{{ nodeCameraList.length }}）</span>
                </template>
                <el-table
                  v-loading="nodeCamerasLoading"
                  :data="pagedNodeCameras"
                  height="320"
                  highlight-current-row
                  size="small"
                >
                  <el-table-column prop="name" label="名称" min-width="120" />
                  <el-table-column prop="catalogGbDeviceId" label="编组国标" min-width="140" />
                  <el-table-column prop="deviceGbId" label="注册国标" min-width="140" />
                  <el-table-column prop="platformName" label="平台" width="100" />
                  <el-table-column label="在线" width="80">
                    <template #default="scope">
                      <el-tag :type="scope.row.online ? 'success' : 'info'" size="small">
                        {{ scope.row.online ? '在线' : '离线' }}
                      </el-tag>
                    </template>
                  </el-table-column>
                  <el-table-column label="操作" width="220" fixed="right">
                    <template #default="scope">
                      <el-button size="small" type="primary" text @click="onPreview(scope.row)">预览</el-button>
                      <el-button size="small" text @click="goReplay(scope.row)">回看</el-button>
                      <el-button size="small" type="danger" text @click="unmountCamera(scope.row)">取消挂载</el-button>
                    </template>
                  </el-table-column>
                </el-table>
                <div v-if="nodeCameraList.length > pageSize" class="table-footer">
                  <el-pagination
                    v-model:current-page="cameraPage"
                    v-model:page-size="pageSize"
                    :page-sizes="[10, 20]"
                    layout="total, sizes, prev, pager, next"
                    :total="nodeCameraList.length"
                    small
                  />
                </div>
                <div v-else-if="nodeCameraList.length === 0" class="empty-tip">当前节点下暂无挂载。</div>
              </el-card>
            </template>
            <template v-else>
              <el-card shadow="hover">
                <div class="empty-tip">请在左侧选择节点。</div>
              </el-card>
            </template>
          </section>
        </div>
      </el-tab-pane>

      <el-tab-pane label="从下级导入" name="import">
        <el-card shadow="hover" class="import-card">
          <el-form inline class="import-form">
            <el-form-item label="目标父节点">
              <el-select
                v-model="importTargetParentId"
                filterable
                placeholder="选本机编组树中的目录节点"
                style="width: 280px"
              >
                <el-option
                  v-for="o in flatGroupOptions"
                  :key="o.id"
                  :label="o.label"
                  :value="o.id"
                />
              </el-select>
            </el-form-item>
            <el-form-item label="下级平台">
              <el-select v-model="importPlatformId" placeholder="选择平台" style="width: 220px" filterable>
                <el-option v-for="p in platforms" :key="p.id" :label="p.name + ' (' + p.id + ')'" :value="String(p.id)" />
              </el-select>
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="importLoading" @click="loadImportTree">加载预览树</el-button>
              <el-button type="success" :disabled="!importTreeRoots.length" @click="submitImport">导入选中项</el-button>
            </el-form-item>
          </el-form>
          <p class="dialog-desc">
            勾选目录或摄像机；将自动包含通往摄像机的目录链。已在本机编组占用的源目录或已挂载的摄像机会显示为勾选且灰色不可再选（表示已导入）。
          </p>
          <el-tree
            v-if="importTreeRoots.length"
            :key="importTreeInstanceKey"
            ref="importTreeRef"
            :data="importTreeRoots"
            node-key="key"
            :props="{ label: 'label', children: 'children', disabled: 'disabled' }"
            show-checkbox
            default-expand-all
            class="import-tree"
          />
          <div v-else class="empty-tip">请选择平台并点击「加载预览树」。</div>
        </el-card>
      </el-tab-pane>

      <el-tab-pane label="下级目录预览" name="readonly">
        <el-card shadow="hover">
          <el-form inline>
            <el-form-item label="平台">
              <el-select v-model="roPlatformId" placeholder="选择" style="width: 240px" filterable @change="loadReadonlyTree">
                <el-option v-for="p in platforms" :key="p.id" :label="p.name" :value="String(p.id)" />
              </el-select>
            </el-form-item>
          </el-form>
          <el-tree
            v-if="readonlyTree.length"
            :data="readonlyTree"
            node-key="key"
            :props="{ label: 'label', children: 'children' }"
            default-expand-all
            class="catalog-tree"
          />
          <div v-else class="empty-tip">请选择平台加载只读目录树（与实时预览同源接口）。</div>
        </el-card>
      </el-tab-pane>
    </el-tabs>

    <el-dialog v-model="nodeDialogVisible" :title="nodeDialogTitle" width="480px">
      <el-form ref="nodeFormRef" :model="nodeForm" :rules="nodeFormRules" label-width="100px">
        <el-form-item label="名称" prop="name">
          <el-input v-model="nodeForm.name" placeholder="节点名称" />
        </el-form-item>
        <el-form-item label="类型" prop="nodeType">
          <el-select v-model="nodeForm.nodeType" style="width: 100%" :disabled="nodeDialogMode === 'edit'">
            <el-option label="业务分组（国标 215）" :value="3" />
            <el-option label="虚拟组织 / 目录（国标 216）" :value="1" />
            <el-option label="行政区域" :value="2" />
            <el-option label="通道占位（少用）" :value="0" />
          </el-select>
        </el-form-item>
        <el-form-item label="行政区划" prop="civilCode">
          <el-input v-model="nodeForm.civilCode" placeholder="可选" />
        </el-form-item>
        <el-form-item label="业务分组 ID" prop="businessGroupId">
          <el-input v-model="nodeForm.businessGroupId" placeholder="可选 BusinessGroupID" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="nodeDialogVisible = false">取消</el-button>
        <el-button type="primary" @click="submitNodeForm">确定</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="assignDialogVisible" title="分配摄像头" width="640px">
      <p class="dialog-desc">
        全量覆盖当前节点挂载。已挂在<strong>其他</strong>编组节点下的摄像机为勾选且灰色不可选；仅未挂载或已挂在本节点的摄像机可改选。
      </p>
      <el-tree
        ref="assignTreeRef"
        :data="assignTreeData"
        node-key="id"
        :props="{ label: 'label', children: 'children', disabled: 'disabled' }"
        show-checkbox
        default-expand-all
        class="assign-tree"
      />
      <template #footer>
        <el-button @click="assignDialogVisible = false">取消</el-button>
        <el-button type="primary" @click="submitAssign">确定</el-button>
      </template>
    </el-dialog>

    <el-dialog
      v-model="previewDialogVisible"
      :title="previewTitle"
      width="800px"
      top="8vh"
      @closed="onPreviewDialogClose"
    >
      <div v-if="currentCamera">
        <div v-if="previewState === 'error'" class="preview-err">{{ previewError }}</div>
        <div ref="playerContainer" class="player-wrap" />
        <PtzPanel
          :camera-id="String(previewRowId || '')"
          :platform-gb-id="String(currentCamera.platformGbId || '')"
          :platform-db-id="currentCamera.platformDbId"
        />
      </div>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, watch, nextTick, onMounted, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import PtzPanel from '@/components/PtzPanel.vue'
import { api } from '@/api/client'
import { buildCatalogForestGb28181 } from '@/utils/catalogTreeGb28181.js'
import { createJessibucaPlayer, destroyJessibucaPlayer } from '@/composables/useJessibuca.js'

const router = useRouter()

const mainTab = ref('group')
const treeLoading = ref(false)
const treeRef = ref(null)
const rawGroupItems = ref([])

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
    civilCode: n.civilCode || '',
    businessGroupId: n.businessGroupId || '',
    sortOrder: n.sortOrder ?? 0,
    children,
  }
}

const treeData = computed(() => (rawGroupItems.value || []).map(mapApiNode))

const hasRoot = computed(() => (rawGroupItems.value || []).some((n) => n.parentId == null))

const flatGroupOptions = computed(() => {
  const out = []
  const walk = (xs, depth) => {
    for (const x of xs || []) {
      const pad = '　'.repeat(depth)
      out.push({ id: String(x.id), label: `${pad}${x.name} (${x.gbDeviceId})` })
      walk(x.children, depth + 1)
    }
  }
  walk(rawGroupItems.value || [], 0)
  return out
})

async function loadCatalogGroupTree() {
  treeLoading.value = true
  try {
    const res = await api.getCatalogGroupNodes({ nested: '1' })
    if (res.code === 0 && res.data?.items) rawGroupItems.value = res.data.items
    else rawGroupItems.value = []
  } catch (e) {
    ElMessage.error(e?.message || '加载编组树失败')
    rawGroupItems.value = []
  } finally {
    treeLoading.value = false
  }
}

const selectedNode = ref(null)

function onNodeClick(data) {
  selectedNode.value = data
  loadNodeCameras()
}

const nodeCameraList = ref([])
const nodeCamerasLoading = ref(false)

async function loadNodeCameras() {
  const node = selectedNode.value
  if (!node?.id) {
    nodeCameraList.value = []
    return
  }
  nodeCamerasLoading.value = true
  try {
    const res = await api.getCatalogGroupNodeCameras(node.id)
    if (res.code === 0 && res.data?.items) {
      nodeCameraList.value = res.data.items.map((r) => ({
        id: r.cameraId,
        cameraId: r.cameraId,
        catalogGbDeviceId: r.catalogGbDeviceId,
        deviceGbId: r.deviceGbId,
        name: r.name,
        online: r.online === true || r.online === 'true',
        platformName: r.platformName,
        platformGbId: r.platformGbId,
        platformDbId: r.platformDbId,
      }))
    } else nodeCameraList.value = []
  } catch {
    nodeCameraList.value = []
  } finally {
    nodeCamerasLoading.value = false
  }
}

const cameraPage = ref(1)
const pageSize = ref(10)
const pagedNodeCameras = computed(() => {
  const list = nodeCameraList.value
  const start = (cameraPage.value - 1) * pageSize.value
  return list.slice(start, start + pageSize.value)
})

onMounted(() => {
  loadCatalogGroupTree()
  loadPlatforms()
})

function nodeTypeLabel(t) {
  if (t === 2) return '行政区域'
  if (t === 0) return '通道占位'
  if (t === 3) return '业务分组'
  return '虚拟组织'
}

const nodeDialogVisible = ref(false)
const nodeDialogMode = ref('add')
const nodeFormRef = ref(null)
const nodeForm = ref({ name: '', nodeType: 1, civilCode: '', businessGroupId: '' })
const nodeFormRules = {
  name: [{ required: true, message: '请输入名称', trigger: 'blur' }],
  nodeType: [{ required: true, message: '请选择类型', trigger: 'change' }],
}

const nodeDialogTitle = computed(() => {
  if (nodeDialogMode.value === 'addChild') return '新增子节点'
  if (nodeDialogMode.value === 'edit') return '编辑节点'
  return '新增根节点'
})

watch(nodeDialogVisible, (v) => {
  if (!v) return
  if (nodeDialogMode.value === 'edit' && selectedNode.value) {
    nodeForm.value = {
      name: selectedNode.value.name,
      nodeType: selectedNode.value.nodeType ?? 1,
      civilCode: selectedNode.value.civilCode || '',
      businessGroupId: selectedNode.value.businessGroupId || '',
    }
  } else {
    const rootAdd = nodeDialogMode.value === 'add'
    nodeForm.value = { name: '', nodeType: rootAdd ? 3 : 1, civilCode: '', businessGroupId: '' }
  }
})

function onAddRoot() {
  selectedNode.value = null
  nodeDialogMode.value = 'add'
  nodeDialogVisible.value = true
}

function onAddChild() {
  if (!selectedNode.value) return
  nodeDialogMode.value = 'addChild'
  nodeDialogVisible.value = true
}

function onEditNode() {
  if (!selectedNode.value) return
  nodeDialogMode.value = 'edit'
  nodeDialogVisible.value = true
}

async function submitNodeForm() {
  try {
    await nodeFormRef.value?.validate()
  } catch {
    return
  }
  const { name, nodeType, civilCode, businessGroupId } = nodeForm.value
  const editId = selectedNode.value?.id
  let newNodeId = null
  try {
    if (nodeDialogMode.value === 'edit') {
      if (!editId) return
      const res = await api.putCatalogGroupNode(editId, {
        name,
        sortOrder: selectedNode.value.sortOrder ?? 0,
        civilCode,
        businessGroupId,
      })
      if (res.code !== 0) {
        ElMessage.error(res.message || '更新失败')
        return
      }
      ElMessage.success('已更新')
    } else {
      const parentId =
        nodeDialogMode.value === 'addChild' && selectedNode.value?.id ? selectedNode.value.id : null
      const body =
        parentId == null
          ? { name, nodeType, civilCode, businessGroupId, parentId: null }
          : { name, nodeType, civilCode, businessGroupId, parentId }
      const res = await api.postCatalogGroupNode(body)
      if (res.code !== 0) {
        ElMessage.error(res.message || '新增失败')
        return
      }
      ElMessage.success('已新增')
      if (res.data?.id != null) newNodeId = String(res.data.id)
    }
    nodeDialogVisible.value = false
    await loadCatalogGroupTree()
    await nextTick()
    if (newNodeId && treeRef.value) {
      treeRef.value.setCurrentKey(newNodeId)
      const find = (xs) => {
        for (const x of xs || []) {
          if (String(x.id) === newNodeId) return x
          const c = find(x.children)
          if (c) return c
        }
        return null
      }
      selectedNode.value = find(treeData.value)
      loadNodeCameras()
    } else if (nodeDialogMode.value === 'edit' && editId) {
      treeRef.value?.setCurrentKey(editId)
      const find = (xs) => {
        for (const x of xs || []) {
          if (String(x.id) === String(editId)) return x
          const c = find(x.children)
          if (c) return c
        }
        return null
      }
      await nextTick()
      selectedNode.value = find(treeData.value)
      loadNodeCameras()
    }
  } catch (e) {
    ElMessage.error(e?.message || '请求失败')
  }
}

function onDeleteNode() {
  const node = selectedNode.value
  if (!node) return
  ElMessageBox.confirm(`确定删除「${node.name}」及其子节点与挂载？`, '确认', { type: 'warning' })
    .then(async () => {
      try {
        const res = await api.deleteCatalogGroupNode(node.id)
        if (res.code !== 0) {
          ElMessage.error(res.message || '删除失败')
          return
        }
        ElMessage.success('已删除')
        selectedNode.value = null
        nodeCameraList.value = []
        await loadCatalogGroupTree()
      } catch (e) {
        ElMessage.error(e?.message || '失败')
      }
    })
    .catch(() => {})
}


const allCameras = ref([])
/** 全库编组挂载索引，用于分配弹窗灰显已挂他处的摄像机 */
const assignMountIndex = ref([])
const assignDialogVisible = ref(false)
const assignTreeRef = ref(null)

const cameraIdSet = computed(() => new Set(allCameras.value.map((c) => String(c.id))))

function assignMountMap() {
  const m = new Map()
  for (const it of assignMountIndex.value) {
    m.set(String(it.cameraId), String(it.groupNodeId))
  }
  return m
}

const assignTreeData = computed(() => {
  const cur = selectedNode.value?.id != null ? String(selectedNode.value.id) : ''
  const mountMap = assignMountMap()
  const byPlatform = {}
  for (const c of allCameras.value) {
    const pid = String(c.platformDbId ?? c.platformId ?? '0')
    if (!byPlatform[pid]) {
      byPlatform[pid] = { platformName: c.platformName || '平台', cameras: [] }
    }
    byPlatform[pid].cameras.push(c)
  }
  return Object.entries(byPlatform).map(([pid, p]) => ({
    id: `p_${pid}`,
    label: p.platformName,
    disabled: true,
    children: p.cameras.map((c) => {
      const gid = mountMap.get(String(c.id)) || ''
      const onOther = Boolean(gid && gid !== cur)
      return {
        id: c.id,
        label: onOther
          ? `${c.name}（${c.deviceGbId || c.id}）（已挂他处）`
          : `${c.name}（${c.deviceGbId || c.id}）`,
        isPlatform: false,
        disabled: onOther,
      }
    }),
  }))
})

async function loadAllCameras() {
  try {
    const res = await api.listCameras({ page: 1, pageSize: 2000 })
    if (res.code === 0 && res.data) allCameras.value = res.data.items || []
    else allCameras.value = []
  } catch {
    allCameras.value = []
  }
}

async function loadCameraMountIndex() {
  try {
    const res = await api.getCatalogGroupCameraMounts()
    if (res.code === 0 && res.data?.items) assignMountIndex.value = res.data.items
    else assignMountIndex.value = []
  } catch {
    assignMountIndex.value = []
  }
}

async function openAssignDialog() {
  if (!selectedNode.value?.id) {
    ElMessage.warning('请先选择节点')
    return
  }
  await Promise.all([loadCameraMountIndex(), loadAllCameras()])
  assignDialogVisible.value = true
  const cur = String(selectedNode.value.id)
  const mmap = assignMountMap()
  await nextTick()
  await nextTick()
  const keySet = new Set()
  for (const c of nodeCameraList.value) keySet.add(c.id)
  for (const [camId, gid] of mmap.entries()) {
    if (gid !== cur) keySet.add(camId)
  }
  assignTreeRef.value?.setCheckedKeys([...keySet], false)
}

async function submitAssign() {
  const node = selectedNode.value
  if (!node?.id) return
  const tree = assignTreeRef.value
  if (!tree) return
  const checked = tree.getCheckedKeys()
  const halfChecked = tree.getHalfCheckedKeys()
  const nodeIdStr = String(node.id)
  const mmap = assignMountMap()
  const cameraIds = [...checked, ...halfChecked]
    .filter((id) => cameraIdSet.value.has(String(id)))
    .filter((id) => {
      const gid = mmap.get(String(id))
      if (gid && gid !== nodeIdStr) return false
      return true
    })
  try {
    await api.putCatalogGroupNodeCameras(node.id, { cameraIds })
    ElMessage.success('已更新挂载')
    assignDialogVisible.value = false
    await loadNodeCameras()
  } catch (e) {
    ElMessage.error(e?.message || '保存失败')
  }
}

async function unmountCamera(row) {
  const node = selectedNode.value
  if (!node?.id) return
  const cameraIds = nodeCameraList.value.filter((c) => String(c.id) !== String(row.id)).map((c) => c.id)
  try {
    await api.putCatalogGroupNodeCameras(node.id, { cameraIds })
    ElMessage.success('已取消挂载')
    await loadNodeCameras()
  } catch (e) {
    ElMessage.error(e?.message || '操作失败')
  }
}

const currentCamera = ref(null)
const previewRowId = ref('')
const previewDialogVisible = ref(false)
const previewState = ref('idle')
const previewError = ref('')
const playerContainer = ref(null)
let playerHandle = null
let currentStreamId = null

const previewTitle = computed(() => {
  if (!currentCamera.value) return '实时预览'
  return `实时预览 - ${currentCamera.value.name}`
})

async function onPreview(row) {
  currentCamera.value = row
  previewRowId.value = String(row.id ?? row.cameraId ?? '')
  previewDialogVisible.value = true
  previewState.value = 'loading'
  previewError.value = ''
  try {
    const res = await api.startPreview(
      previewRowId.value,
      row.platformGbId,
      row.platformDbId,
      row.deviceGbId,
    )
    if (res.code !== 0) {
      previewState.value = 'error'
      previewError.value = res.message || '发起点播失败'
      ElMessage.error(previewError.value)
      return
    }
    currentStreamId = res.data?.streamId
    const flvUrl = res.data?.flvUrl
    /* el-dialog 内容区可能晚一帧才有非零尺寸，双 nextTick 与 composable 内 wait 叠加更稳 */
    await nextTick()
    await nextTick()
    destroyPlayer()
    playerHandle = await createJessibucaPlayer(playerContainer.value, flvUrl, {
      onError: (err) => {
        previewState.value = 'error'
        previewError.value = String(err?.message || err)
      },
    })
    previewState.value = 'playing'
  } catch (e) {
    previewState.value = 'error'
    previewError.value = e?.message || '连接失败'
    ElMessage.error(previewError.value)
  }
}

function destroyPlayer() {
  if (playerHandle) {
    destroyJessibucaPlayer(playerHandle)
    playerHandle = null
  }
}

function onPreviewDialogClose() {
  destroyPlayer()
  previewState.value = 'idle'
  previewError.value = ''
  currentStreamId = null
  currentCamera.value = null
  previewRowId.value = ''
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

const platforms = ref([])
async function loadPlatforms() {
  try {
    const res = await api.listDevicePlatforms({ page: 1, pageSize: 100 })
    if (res.code === 0 && res.data?.items) platforms.value = res.data.items
    else platforms.value = []
  } catch {
    platforms.value = []
  }
}

const importPlatformId = ref('')
const importTargetParentId = ref('')
const importLoading = ref(false)
const importTreeRoots = ref([])
/** 每次成功加载递增，强制销毁并重建 el-tree，避免 store.defaultCheckedKeys 与 updateChildren 残留旧勾选 */
const importTreeInstanceKey = ref(0)
const importTreeRef = ref(null)
const importCatalogItems = ref([])
const importPlatformGbId = ref('')
const importParentOf = ref(new Map())

function treeParentMap(roots, parentId = null, m = new Map()) {
  for (const n of roots || []) {
    if (parentId) m.set(n.nodeId, parentId)
    if (n.children?.length) treeParentMap(n.children, n.nodeId, m)
  }
  return m
}

/**
 * 下级目录项 -> 本机编组 node_type（发号类型位）；215=业务分组 3，216=虚拟组织 1，其余目录默认 216。
 */
function catalogGroupNodeTypeFromCatalogItem(it) {
  const n = Number(it.type)
  if (n === 2) return 2
  if (n === 0) return 0
  const sid = String(it.nodeId || '').trim()
  if (sid.length >= 13) {
    const t3 = sid.substring(10, 13)
    if (t3 === '215') return 3
    if (t3 === '216') return 1
  }
  return 1
}

function nearestDirAncestor(nodeId, parentOf, byId) {
  let p = parentOf.get(nodeId)
  while (p) {
    const it = byId.get(p)
    if (it && Number(it.type) !== 0) return p
    p = parentOf.get(p)
  }
  return ''
}

async function loadImportTree() {
  if (!importPlatformId.value) {
    ElMessage.warning('请选择平台')
    return
  }
  importLoading.value = true
  importTreeRoots.value = []
  try {
    const plat = platforms.value.find((p) => String(p.id) === String(importPlatformId.value))
    importPlatformGbId.value = plat?.gbId || ''
    const [treeRes, occRes, camRes] = await Promise.all([
      api.getCatalogTree(importPlatformId.value),
      api.getCatalogGroupImportOccupancy(importPlatformId.value),
      api.listCameras({ page: 1, pageSize: 3000, platformId: importPlatformId.value }),
    ])
    const items = treeRes.data?.items || []
    importCatalogItems.value = items
    const byId = new Map(items.map((it) => [it.nodeId, it]))
    const forest = buildCatalogForestGb28181(items, {
      platformDbId: importPlatformId.value,
      platformGbId: importPlatformGbId.value,
    })
    importParentOf.value = treeParentMap(forest)
    const occSrc = new Set((occRes.data?.sourceGbDeviceIds || []).map(String))
    const occCam = new Set((occRes.data?.cameraIds || []).map(String))
    const cams = camRes.data?.items || []
    const devToCam = new Map()
    for (const c of cams) {
      const k = String(c.deviceGbId || '').trim()
      if (k) devToCam.set(k, String(c.id))
    }
    function markDisabled(nodes) {
      for (const n of nodes || []) {
        n.alreadyImported = false
        if (n.isDevice) {
          const cid = devToCam.get(String(n.nodeId).trim())
          n.cameraDbId = cid
          const missingCam = !cid
          const camImported = Boolean(cid && occCam.has(String(cid)))
          n.disabled = missingCam || camImported
          if (camImported) {
            n.alreadyImported = true
            n.label = `${n.baseLabel}（已导入）`
          } else if (missingCam) {
            n.label = `${n.baseLabel}（未收编）`
          } else {
            n.label = n.baseLabel
          }
        } else {
          const dirImported = occSrc.has(String(n.nodeId))
          n.disabled = dirImported
          if (dirImported) {
            n.alreadyImported = true
            n.label = `${n.baseLabel}（已导入）`
          } else {
            n.label = n.baseLabel
          }
        }
        if (n.children?.length) markDisabled(n.children)
      }
    }
    markDisabled(forest)
    importTreeInstanceKey.value += 1
    importTreeRoots.value = forest
    await nextTick()
    await nextTick()
    const importedKeys = []
    const collectImportedKeys = (ns) => {
      for (const x of ns || []) {
        if (x.alreadyImported) importedKeys.push(x.key)
        if (x.children?.length) collectImportedKeys(x.children)
      }
    }
    collectImportedKeys(forest)
    const tr = importTreeRef.value
    if (tr) {
      tr.setCheckedKeys([], false)
      tr.setCheckedKeys(importedKeys, false)
    }
  } catch (e) {
    ElMessage.error(e?.message || '加载失败')
  } finally {
    importLoading.value = false
  }
}

async function submitImport() {
  if (!importTargetParentId.value) {
    ElMessage.warning('请选择目标父节点')
    return
  }
  const tree = importTreeRef.value
  if (!tree) return
  // 已导入项虽显示为勾选且 disabled，不得参与提交
  const nodes = tree.getCheckedNodes(false, true).filter((n) => !n.disabled)
  const checkedDev = new Set()
  const checkedDir = new Set()
  for (const n of nodes) {
    if (n.isDevice && n.cameraDbId) checkedDev.add(n.nodeId)
    if (!n.isDevice) checkedDir.add(n.nodeId)
  }
  if (checkedDev.size === 0 && checkedDir.size === 0) {
    ElMessage.warning('请勾选要导入的目录或摄像机')
    return
  }
  const parentOf = importParentOf.value
  const byId = new Map(importCatalogItems.value.map((it) => [it.nodeId, it]))
  const dirSources = new Set(checkedDir)
  for (const devId of checkedDev) {
    let cur = parentOf.get(devId)
    while (cur) {
      dirSources.add(cur)
      cur = parentOf.get(cur)
    }
  }
  for (const d of checkedDir) {
    let cur = parentOf.get(d)
    while (cur) {
      dirSources.add(cur)
      cur = parentOf.get(cur)
    }
  }
  const directories = []
  for (const sid of dirSources) {
    const it = byId.get(sid)
    if (!it || Number(it.type) === 0) continue
    const p = parentOf.get(sid)
    let parentSrc = ''
    if (p && dirSources.has(p)) parentSrc = p
    directories.push({
      sourceGbDeviceId: sid,
      name: it.name || sid,
      nodeType: catalogGroupNodeTypeFromCatalogItem(it),
      parentSourceGbDeviceId: parentSrc || null,
    })
  }
  const mounts = []
  const devNodeById = new Map()
  for (const n of nodes) {
    if (n.isDevice && n.nodeId) devNodeById.set(n.nodeId, n)
  }
  for (const devId of checkedDev) {
    const camId = devNodeById.get(devId)?.cameraDbId
    if (!camId) continue
    const pdir = nearestDirAncestor(devId, parentOf, byId)
    mounts.push({
      cameraId: Number(camId),
      sourceDeviceGbId: devId,
      parentSourceGbDeviceId: pdir && dirSources.has(pdir) ? pdir : null,
    })
  }
  try {
    const res = await api.postCatalogGroupImport({
      targetParentId: importTargetParentId.value,
      platformDbId: importPlatformId.value,
      directories,
      mounts,
    })
    if (res.code !== 0) {
      ElMessage.error(res.message || '导入失败')
      return
    }
    ElMessage.success(`已导入目录 ${res.data?.importedDirectories ?? 0}、摄像机 ${res.data?.importedMounts ?? 0}`)
    await loadCatalogGroupTree()
    mainTab.value = 'group'
  } catch (e) {
    ElMessage.error(e?.message || '导入失败')
  }
}

const roPlatformId = ref('')
const readonlyTree = ref([])

async function loadReadonlyTree() {
  readonlyTree.value = []
  if (!roPlatformId.value) return
  try {
    const plat = platforms.value.find((p) => String(p.id) === String(roPlatformId.value))
    const res = await api.getCatalogTree(roPlatformId.value)
    const items = res.data?.items || []
    readonlyTree.value = buildCatalogForestGb28181(items, {
      platformDbId: roPlatformId.value,
      platformGbId: plat?.gbId || '',
    })
  } catch (e) {
    ElMessage.error(e?.message || '加载失败')
  }
}

onUnmounted(() => {
  destroyPlayer()
})
</script>

<style scoped>
.page-header {
  margin-bottom: 16px;
}

.catalog-tabs {
  margin-top: 8px;
}

.catalog-layout {
  display: flex;
  gap: 20px;
  align-items: flex-start;
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
}

.tree-toolbar {
  margin-bottom: 12px;
  display: flex;
  gap: 8px;
}

.detail-card .assign-btn {
  margin-left: 8px;
}

.cameras-card .table-footer {
  margin-top: 12px;
  display: flex;
  justify-content: flex-end;
}

.empty-tip {
  padding: 24px 0;
  text-align: center;
  color: var(--gb-text-soft, #909399);
  font-size: 13px;
}

.assign-tree,
.import-tree {
  max-height: 420px;
  overflow-y: auto;
}

.assign-tree :deep(.el-checkbox.is-disabled.is-checked .el-checkbox__inner) {
  background-color: var(--el-checkbox-checked-bg-color, var(--el-color-primary));
  border-color: var(--el-checkbox-checked-input-border-color, var(--el-color-primary));
}
.assign-tree :deep(.el-checkbox.is-disabled.is-checked .el-checkbox__inner::after) {
  border-color: var(--el-checkbox-checked-icon-color, var(--el-color-white));
}

.dialog-desc {
  font-size: 13px;
  color: var(--gb-text-soft, #909399);
  margin-bottom: 12px;
}

/* 与 Live 页一致：Jessibuca 开源版以 canvas 为主，容器须固定宽高并拉伸内部 canvas，勿用 flex 居中（否则画面无法铺满或尺寸为 0） */
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

.import-form {
  margin-bottom: 8px;
}

/* 禁用但已勾选（已导入）：保持勾号可见，与「未收编」灰色未选区分 */
.import-tree :deep(.el-checkbox.is-disabled.is-checked .el-checkbox__inner) {
  background-color: var(--el-checkbox-checked-bg-color, var(--el-color-primary));
  border-color: var(--el-checkbox-checked-input-border-color, var(--el-color-primary));
}
.import-tree :deep(.el-checkbox.is-disabled.is-checked .el-checkbox__inner::after) {
  border-color: var(--el-checkbox-checked-icon-color, var(--el-color-white));
}
</style>

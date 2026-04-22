<!--
  文件名: Devices.vue
  功能: 下级设备平台管理页面
  描述:
    - 管理下级平台/设备的接入信息
    - 支持黑白名单策略配置（normal/whitelist/blacklist）
    - 支持独立配置模式（inherit/custom）
    - 支持自定义鉴权密码和媒体流地址
    - 提供 Catalog 目录查询功能
    - 支持分页、筛选、增删改查操作
  依赖: Element Plus 组件库、Vue 3 Composition API
  注意事项:
    - 平台 ID 必须符合 GB28181 标准格式
    - 删除平台会级联删除其下所有摄像头数据
-->
<template>
  <div class="page platforms">
    <header class="page-header">
      <div class="page-header-main">
        <div>
          <h2 class="page-title">平台管理</h2>
          <p class="page-subtitle">
            统一管理下级平台的接入信息与接入策略。
          </p>
        </div>
      </div>
      <div class="page-header-actions">
        <el-button type="primary" @click="openAddDialog">
          新增下级平台
        </el-button>
      </div>
    </header>

    <section class="page-section">
      <el-card shadow="hover">
        <template #header>
          <span>筛选条件</span>
        </template>
        <div class="toolbar">
          <el-input
            v-model="searchInput"
            clearable
            class="toolbar-search"
            placeholder="按名称或 ID 搜索平台"
            @keyup.enter="onSearch"
            @clear="onResetFilters"
          />
          <el-select
            v-model="filterOnline"
            clearable
            placeholder="在线状态"
            class="toolbar-select"
          >
            <el-option label="全部状态" value="" />
            <el-option label="在线" value="online" />
            <el-option label="离线" value="offline" />
          </el-select>
          <el-select
            v-model="filterListMode"
            clearable
            placeholder="名单类型"
            class="toolbar-select"
          >
            <el-option label="全部类型" value="" />
            <el-option label="普通" value="normal" />
            <el-option label="白名单" value="whitelist" />
            <el-option label="黑名单" value="blacklist" />
          </el-select>
          <el-select
            v-model="filterUseMode"
            clearable
            placeholder="配置模式"
            class="toolbar-select"
          >
            <el-option label="全部模式" value="" />
            <el-option label="跟随系统" value="system" />
            <el-option label="独立配置" value="independent" />
          </el-select>
          <div class="toolbar-spacer"></div>
          <el-button
            type="primary"
            class="toolbar-search-button"
            @click="onSearch"
          >
            搜索
          </el-button>
          <el-button
            class="toolbar-reset-button"
            @click="onResetFilters"
          >
            重置
          </el-button>
        </div>
      </el-card>
    </section>

    <section class="page-section">
      <el-card shadow="hover">
        <template #header>
          <span>平台列表</span>
        </template>
        <el-table
          v-loading="loading"
          :data="pagedPlatforms"
          highlight-current-row
          style="width: 100%"
          @current-change="onRowChange"
        >
          <el-table-column type="index" label="序号" width="50" :index="(index) => (currentPage - 1) * pageSize + index + 1" />
          <el-table-column prop="name" label="平台名称" min-width="140" show-overflow-tooltip />
          <el-table-column prop="gbId" label="平台 ID" min-width="180" show-overflow-tooltip />
          <el-table-column label="在线状态" width="80">
            <template #default="scope">
              <el-tag :type="scope.row.online ? 'success' : 'info'" size="small">
                {{ scope.row.online ? '在线' : '离线' }}
              </el-tag>
            </template>
          </el-table-column>
          <el-table-column label="名单类型" width="100">
            <template #default="scope">
              {{ getListModeLabel(scope.row.listMode) }}
            </template>
          </el-table-column>
          <el-table-column label="配置模式" width="100">
            <template #default="scope">
              {{ getUseModeLabel(scope.row.useMode) }}
            </template>
          </el-table-column>
          <el-table-column prop="cameraCount" label="摄像头总数" width="100" />
          <el-table-column label="对端出口" min-width="140" show-overflow-tooltip>
            <template #default="scope">
              {{ formatSignalEndpoint(scope.row) }}
            </template>
          </el-table-column>
          <el-table-column label="创建时间" min-width="160" show-overflow-tooltip>
            <template #default="scope">
              {{ formatDateTime(scope.row.createdAt) }}
            </template>
          </el-table-column>
          <el-table-column label="最后心跳" min-width="160" show-overflow-tooltip>
            <template #default="scope">
              {{ formatDateTime(scope.row.lastHeartbeatAt) }}
            </template>
          </el-table-column>
          <el-table-column label="操作" min-width="260" fixed="right">
            <template #default="scope">
              <el-button type="success" size="small" text @click.stop="onQueryCatalog(scope.row)"
                :loading="scope.row.catalogQuerying">
                查询设备目录
              </el-button>
              <el-button type="info" size="small" text @click.stop="onViewCatalogTree(scope.row)">
                查看目录树
              </el-button>
              <el-button type="primary" size="small" text @click.stop="onEditPlatform(scope.row)">
                编辑
              </el-button>
              <el-button type="danger" size="small" text @click.stop="onDeletePlatform(scope.row)">
                删除
              </el-button>
            </template>
          </el-table-column>
        </el-table>
        <div class="table-footer">
          <el-pagination
            v-model:current-page="currentPage"
            v-model:page-size="pageSize"
            :page-sizes="[5, 10, 20]"
            layout="total, sizes, prev, pager, next"
            :total="total"
            @current-change="loadPlatforms"
            @size-change="loadPlatforms"
          />
        </div>
      </el-card>
    </section>

    <!-- 目录树查看对话框 -->
    <el-dialog
      v-model="catalogTreeVisible"
      :title="catalogTreePlatform ? `目录树 - ${catalogTreePlatform.name}` : '目录树'"
      width="600px"
    >
      <el-tree
        v-loading="catalogTreeLoading"
        :data="catalogTreeData"
        :props="{ label: 'label', children: 'children' }"
        default-expand-all
        highlight-current
        class="catalog-tree-dialog"
      >
        <template #default="{ node, data }">
          <span class="tree-node">
            <el-tag :type="getNodeTagType(data.type)" size="small" class="node-type-tag">
              {{ getNodeTypeLabel(data.type) }}
            </el-tag>
            <span class="node-name">{{ data.name }}</span>
            <span v-if="data.manufacturer" class="node-info">({{ data.manufacturer }})</span>
            <el-tag v-if="data.status === 'ON' || data.status === 'OK'" type="success" size="small" effect="plain">
              在线
            </el-tag>
          </span>
        </template>
      </el-tree>
      <template #footer>
        <el-button @click="catalogTreeVisible = false">关闭</el-button>
      </template>
    </el-dialog>

    <el-dialog
      v-model="addDialogVisible"
      :title="editingPlatformId ? '编辑下级平台' : '新增下级平台'"
      width="520px"
    >
      <el-form :model="addForm" label-width="100px" class="add-form">
        <el-form-item label="平台名称">
          <el-input v-model="addForm.name" />
        </el-form-item>
        <el-form-item label="平台 ID">
          <el-input v-model="addForm.id" />
        </el-form-item>
        <el-form-item label="配置模式">
          <el-radio-group v-model="addForm.useMode">
            <el-radio :label="'system'">跟随系统配置</el-radio>
            <el-radio :label="'independent'">独立配置</el-radio>
          </el-radio-group>
        </el-form-item>
        <template v-if="addForm.useMode === 'independent'">
          <el-form-item label="信令地址">
            <el-input v-model="addForm.addr" placeholder="例如 192.168.1.100:5060" />
          </el-form-item>
          <el-form-item label="鉴权账号">
            <el-input v-model="addForm.authUser" />
          </el-form-item>
          <el-form-item label="鉴权密码">
            <el-input
              v-model="addForm.authPassword"
              type="password"
              show-password
            />
          </el-form-item>
          <el-form-item label="流媒体传输协议">
            <el-select v-model="addForm.streamRtpTransport" placeholder="跟随系统" clearable style="width: 100%">
              <el-option label="跟随系统默认" value="" />
              <el-option label="UDP" value="udp" />
              <el-option label="TCP" value="tcp" />
            </el-select>
            <p class="form-item-hint">仅影响对下级点播 INVITE 的 SDP 与 ZLM 收流；未选时采用系统配置。</p>
          </el-form-item>
          <el-form-item label="流媒体地址">
            <el-input
              v-model="addForm.mediaUrl"
              placeholder="INVITE SDP 中 c=/o= 连接地址（IP 或 http://host/...）；留空则用系统流媒体 IP"
            />
          </el-form-item>
        </template>
        <el-form-item label="名单类型">
          <el-select v-model="addForm.listMode" placeholder="请选择">
            <el-option label="普通" value="normal" />
            <el-option label="白名单" value="whitelist" />
            <el-option label="黑名单" value="blacklist" />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <div class="add-actions">
          <el-button @click="addDialogVisible = false">取消</el-button>
          <el-button type="primary" @click="onSaveNewPlatform" :loading="addSaving">
            保存
          </el-button>
        </div>
      </template>
    </el-dialog>
    <el-dialog
      v-model="strategyDialogVisible"
      title="平台接入详情与策略（占位）"
      width="640px"
    >
      <div v-if="current">
        <el-descriptions :column="2" border class="detail-descriptions">
          <el-descriptions-item label="平台名称">
            {{ current.name }}
          </el-descriptions-item>
            <el-descriptions-item label="平台 ID">
              {{ current.gbId ?? current.id }}
            </el-descriptions-item>
        </el-descriptions>

        <el-form
          :model="strategyForm"
          label-width="120px"
          class="strategy-form"
        >
          <el-form-item label="使用配置">
            <el-radio-group v-model="strategyForm.useMode">
              <el-radio label="system">跟随系统配置</el-radio>
              <el-radio label="independent">独立配置</el-radio>
            </el-radio-group>
          </el-form-item>
          <el-form-item label="名单类型">
            <el-select v-model="strategyForm.listMode" placeholder="请选择">
              <el-option label="白名单" value="whitelist" />
              <el-option label="黑名单" value="blacklist" />
            </el-select>
          </el-form-item>
          <el-form-item label="白名单免鉴权">
            <el-switch
              v-model="strategyForm.noAuth"
              :disabled="strategyForm.listMode !== 'whitelist'"
            />
          </el-form-item>
          <el-form-item label="鉴权账号">
            <el-input v-model="strategyForm.authUser" />
          </el-form-item>
          <el-form-item label="鉴权密码">
            <el-input
              v-model="strategyForm.authPassword"
              type="password"
              show-password
            />
          </el-form-item>
          <el-form-item label="流媒体地址">
            <el-input
              v-model="strategyForm.mediaUrl"
              placeholder="例如 rtp:// 或 rtsp:// 地址（占位）"
            />
          </el-form-item>
          <el-form-item>
            <div class="strategy-actions">
              <span class="hint">
                未配置独立参数时将使用系统默认配置。
              </span>
              <el-button
                type="primary"
                size="small"
                @click="onSaveStrategy"
                :loading="strategySaving"
              >
                保存策略
              </el-button>
            </div>
          </el-form-item>
        </el-form>
      </div>
      <div v-else class="empty-tip">
        当前暂无选中平台，请先在平台列表中选择一行。
      </div>
    </el-dialog>
  </div>
</template>

<script setup>
/**
 * @module Devices
 * @description 下级设备平台管理页面
 * @requires vue
 * @requires element-plus
 * @requires @/api/client
 */
import { ref, computed, reactive, onMounted, watch } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { api } from '@/api/client'
import { formatLocalDateTime as formatDateTime } from '@/utils/dateDisplay'

// ============ 响应式数据 ============

/** 搜索关键词 */
const searchInput = ref('')
/** 在线状态筛选 */
const filterOnline = ref('')
/** 名单类型筛选 */
const filterListMode = ref('')
/** 配置模式筛选 */
const filterUseMode = ref('')

/** 平台列表数据 */
const platforms = ref([])
/** 总记录数 */
const total = ref(0)
/** 加载状态 */
const loading = ref(false)

/** 当前页码 */
const currentPage = ref(1)
/** 每页大小 */
const pageSize = ref(10)

/** 当前选中的平台 */
const current = ref(null)

/** 策略配置对话框可见性 */
const strategyDialogVisible = ref(false)

/** 策略配置表单 */
const strategyForm = reactive({
  useMode: 'system',
  listMode: 'whitelist',
  noAuth: false,
  authUser: '',
  authPassword: '',
  mediaUrl: '',
})

/** 新增/编辑对话框可见性 */
const addDialogVisible = ref(false)
/** 新增保存状态 */
const addSaving = ref(false)
/** 正在编辑的平台 ID */
const editingPlatformId = ref(null)
/** 新增/编辑表单 */
const addForm = reactive({
  name: '',
  id: '',
  useMode: 'system', // 'system' 跟随系统, 'independent' 独立配置
  addr: '',
  authUser: '',
  authPassword: '',
  mediaUrl: '',
  /** 独立配置：''=跟随系统，udp/tcp=覆盖 */
  streamRtpTransport: '',
  listMode: 'normal',
})

/** 策略保存状态 */
const strategySaving = ref(false)

// ============ 计算属性 ============

const filteredPlatforms = computed(() => platforms.value)
const pagedPlatforms = computed(() => platforms.value)

// ============ 方法 ============

/**
 * @description 映射后端平台数据到前端格式
 * @param {Object} item - 后端平台数据
 * @returns {Object} 前端平台对象
 */
function mapPlatformItem(item) {
  return {
    id: item.id,
    name: item.name,
    gbId: item.gbId,
    online: !!item.online,
    listMode: item.listType || 'normal',
    useMode: item.strategyMode === 'custom' ? 'independent' : 'system',
    cameraCount: item.cameraCount ?? 0,
    strategyMode: item.strategyMode || 'inherit',
    customAuthPassword: item.customAuthPassword || '',
    customMediaHost: item.customMediaHost || '',
    customMediaPort: item.customMediaPort || 0,
    streamMediaUrl: item.streamMediaUrl || '',
    streamRtpTransport: item.streamRtpTransport === 'tcp' || item.streamRtpTransport === 'udp' ? item.streamRtpTransport : null,
    contactIp: item.contactIp || '',
    contactPort: item.contactPort ?? 0,
    signalSrcIp: item.signalSrcIp || '',
    signalSrcPort: item.signalSrcPort ?? 0,
    createdAt: item.createdAt || '',
    lastHeartbeatAt: item.lastHeartbeatAt || '',
    catalogQuerying: false,
  }
}

/**
 * @param {{ signalSrcIp?: string, signalSrcPort?: number, contactIp?: string, contactPort?: number }} row
 */
function formatSignalEndpoint(row) {
  if (row.signalSrcIp && row.signalSrcPort) {
    return `${row.signalSrcIp}:${row.signalSrcPort}`
  }
  if (row.contactIp && row.contactPort) {
    return `${row.contactIp}:${row.contactPort}（Contact）`
  }
  return '—'
}

/**
 * @description 加载平台列表
 * @async
 * @returns {Promise<void>}
 * @throws {Error} 加载失败时显示错误消息
 */
async function loadPlatforms() {
  loading.value = true
  try {
    const params = {
      page: currentPage.value,
      pageSize: pageSize.value,
    }
    const keyword = searchInput.value.trim()
    if (keyword) params.keyword = keyword
    if (filterListMode.value === 'whitelist') params.whitelist = 'true'
    if (filterListMode.value === 'blacklist') params.blacklist = 'true'
    if (filterUseMode.value === 'independent') params.mode = 'custom'
    if (filterUseMode.value === 'system') params.mode = 'inherit'
    if (filterOnline.value === 'online') params.online = 'true'
    if (filterOnline.value === 'offline') params.online = 'false'
    const res = await api.listDevicePlatforms(params)
    if (res.code === 0 && res.data) {
      platforms.value = (res.data.items || []).map(mapPlatformItem)
      total.value = res.data.total ?? 0
    }
  } catch (e) {
    ElMessage.error(e?.message || '加载平台列表失败')
    platforms.value = []
    total.value = 0
  } finally {
    loading.value = false
  }
}

function getListModeLabel(mode) {
  if (mode === 'whitelist') return '白名单'
  if (mode === 'blacklist') return '黑名单'
  if (mode === 'normal' || !mode) return '普通'
  return String(mode)
}

function getUseModeLabel(mode) {
  if (mode === 'system' || !mode) return '跟随系统'
  if (mode === 'independent') return '独立配置'
  return String(mode)
}

function onSearch() {
  currentPage.value = 1
  loadPlatforms()
}

function onResetFilters() {
  searchInput.value = ''
  filterOnline.value = ''
  filterListMode.value = ''
  filterUseMode.value = ''
  currentPage.value = 1
  loadPlatforms()
}

onMounted(loadPlatforms)
watch([currentPage, pageSize], () => { loadPlatforms() })

function onRowChange(row) {
  current.value = row || null
  if (row) {
    strategyForm.useMode = row.useMode || 'system'
    strategyForm.listMode = row.listMode === 'normal' ? 'whitelist' : row.listMode || 'whitelist'
    strategyForm.noAuth = false
    strategyForm.authUser = ''
    strategyForm.authPassword = ''
    strategyForm.mediaUrl = ''
  }
}

function onEditStrategy(row) {
  onRowChange(row)
  strategyDialogVisible.value = true
}

function parseMediaUrl(url) {
  if (!url || !url.trim()) return { host: '', port: 0 }
  const s = String(url).trim()
  const idx = s.lastIndexOf(':')
  if (idx <= 0) return { host: s, port: 0 }
  const host = s.slice(0, idx).trim()
  const port = parseInt(s.slice(idx + 1), 10)
  return { host, port: Number.isFinite(port) ? port : 0 }
}

async function onSaveStrategy() {
  if (!current.value) return
  strategySaving.value = true
  try {
    const { host, port } = parseMediaUrl(strategyForm.mediaUrl)
    await api.putDevicePlatform(current.value.id, {
      listType: strategyForm.listMode,
      strategyMode: strategyForm.useMode === 'independent' ? 'custom' : 'inherit',
      customAuthPassword: strategyForm.authPassword,
      customMediaHost: host || undefined,
      customMediaPort: port > 0 ? port : undefined,
    })
    ElMessage.success('策略已保存')
    strategyDialogVisible.value = false
    await loadPlatforms()
  } catch (e) {
    ElMessage.error(e?.message || '保存策略失败')
  } finally {
    strategySaving.value = false
  }
}

function openAddDialog() {
  editingPlatformId.value = null
  addForm.name = ''
  addForm.id = ''
  addForm.useMode = 'system'
  addForm.addr = ''
  addForm.authUser = ''
  addForm.authPassword = ''
  addForm.mediaUrl = ''
  addForm.streamRtpTransport = ''
  addForm.listMode = 'normal'
  addDialogVisible.value = true
}

function onEditPlatform(row) {
  editingPlatformId.value = row.id
  addForm.name = row.name ?? ''
  addForm.id = row.gbId ?? row.id ?? ''
  addForm.useMode = row.strategyMode === 'custom' ? 'independent' : 'system'
  // 解析信令地址
  if (row.customMediaHost && row.customMediaPort) {
    addForm.addr = `${row.customMediaHost}:${row.customMediaPort}`
  } else if (row.customMediaHost) {
    addForm.addr = row.customMediaHost
  } else {
    addForm.addr = ''
  }
  addForm.authUser = ''
  addForm.authPassword = row.customAuthPassword ?? ''
  addForm.mediaUrl = row.streamMediaUrl ?? ''
  addForm.streamRtpTransport =
    row.streamRtpTransport === 'tcp' || row.streamRtpTransport === 'udp' ? row.streamRtpTransport : ''
  addForm.listMode = row.listType ?? 'normal'
  addDialogVisible.value = true
}

async function onDeletePlatform(row) {
  try {
    await ElMessageBox.confirm(`确定删除下级平台「${row.name}」？删除后其下摄像头将解除关联。`, '删除确认', {
      type: 'warning',
    })
  } catch {
    return
  }
  try {
    await api.deleteDevicePlatform(row.id)
    ElMessage.success('已删除')
    if (current.value?.id === row.id) current.value = null
    await loadPlatforms()
  } catch (e) {
    ElMessage.error(e?.message || '删除失败')
  }
}

async function onQueryCatalog(row) {
  if (row.catalogQuerying) return
  
  row.catalogQuerying = true
  try {
    const res = await api.queryCatalog(row.id)
    if (res && res.code === 0) {
      ElMessage.success('设备目录查询请求已发送，请稍后查看结果')
    } else {
      ElMessage.warning(res?.message || '查询请求发送失败')
    }
  } catch (e) {
    ElMessage.error(e?.message || '查询设备目录失败')
  } finally {
    row.catalogQuerying = false
  }
}

async function onSaveNewPlatform() {
  const name = (addForm.name || '').trim()
  const gbId = (addForm.id || '').trim()
  if (!name || !gbId) {
    ElMessage.warning('请填写平台名称和平台 ID')
    return
  }
  addSaving.value = true
  try {
    const { host, port } = parseMediaUrl(addForm.addr)
    const isIndependent = addForm.useMode === 'independent'
    const payload = {
      name,
      gbId,
      listType: addForm.listMode || 'normal',
      strategyMode: isIndependent ? 'custom' : 'inherit',
      customAuthPassword: isIndependent ? (addForm.authPassword || undefined) : undefined,
      customMediaHost: isIndependent ? (host || undefined) : undefined,
      customMediaPort: isIndependent && port > 0 ? port : undefined,
      streamMediaUrl: isIndependent ? (addForm.mediaUrl || undefined) : undefined,
    }
    if (isIndependent) {
      if (addForm.streamRtpTransport === 'tcp' || addForm.streamRtpTransport === 'udp') {
        payload.streamRtpTransport = addForm.streamRtpTransport
      } else {
        payload.streamRtpTransport = null
      }
    }
    if (editingPlatformId.value) {
      await api.putDevicePlatform(editingPlatformId.value, payload)
      ElMessage.success('已更新')
    } else {
      await api.postDevicePlatform(payload)
      ElMessage.success('已添加下级平台')
    }
    addDialogVisible.value = false
    await loadPlatforms()
  } catch (e) {
    ElMessage.error(e?.message || '保存失败')
  } finally {
    addSaving.value = false
  }
}

// ============ 目录树查看功能 ============
const catalogTreeVisible = ref(false)
const catalogTreeLoading = ref(false)
const catalogTreeData = ref([])
const catalogTreePlatform = ref(null)

async function onViewCatalogTree(row) {
  catalogTreePlatform.value = row
  catalogTreeVisible.value = true
  catalogTreeLoading.value = true
  
  try {
    const res = await api.getCatalogTree(row.id)
    if (res && res.code === 0) {
      // 将扁平列表转换为树形结构
      catalogTreeData.value = buildTree(res.data?.items || [])
    } else {
      ElMessage.warning(res?.message || '获取目录树失败')
      catalogTreeData.value = []
    }
  } catch (e) {
    ElMessage.error(e?.message || '获取目录树失败')
    catalogTreeData.value = []
  } finally {
    catalogTreeLoading.value = false
  }
}

// 将扁平列表转换为树形结构
function buildTree(items) {
  const nodeMap = {}
  const roots = []
  
  // 首先创建所有节点的映射
  items.forEach(item => {
    nodeMap[item.nodeId] = {
      id: item.nodeId,
      label: `${item.name} (${getNodeTypeLabel(item.type)})`,
      name: item.name,
      type: item.type,
      typeName: item.typeName,
      manufacturer: item.manufacturer,
      model: item.model,
      status: item.status,
      children: []
    }
  })
  
  // 然后构建父子关系
  items.forEach(item => {
    const node = nodeMap[item.nodeId]
    if (item.parentId && nodeMap[item.parentId]) {
      nodeMap[item.parentId].children.push(node)
    } else {
      // 如果没有父节点或父节点不在列表中，作为根节点
      roots.push(node)
    }
  })
  
  return roots
}

function getNodeTypeLabel(type) {
  const labels = { 0: '设备', 1: '目录', 2: '行政区域' }
  return labels[type] || '未知'
}

function getNodeTagType(type) {
  const types = { 0: 'success', 1: 'primary', 2: 'warning' }
  return types[type] || 'info'
}
</script>

<style scoped>
/* 主内容区与实时预览墙、系统配置一致：铺满 app-main，不限制 max-width */


.page-header {
  margin-bottom: 16px;
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
}

.toolbar-search {
  flex: 1 1 260px;
  max-width: 360px;
}

.toolbar-select {
  width: 140px;
}

.toolbar-spacer {
  flex: 1 1 auto;
}

.toolbar-search-button,
.toolbar-reset-button {
  white-space: nowrap;
}

.table-footer {
  margin-top: 12px;
  display: flex;
  justify-content: flex-end;
}

.detail-descriptions {
  margin-bottom: 12px;
}

.strategy-form {
  max-width: 560px;
}

.strategy-actions {
  width: 100%;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  row-gap: 8px;
}

.strategy-actions .hint {
  max-width: 60%;
  font-size: 12px;
  color: var(--gb-text-soft);
}

.empty-tip {
  padding: 40px 0;
  text-align: center;
  color: #909399;
}

.add-form {
  max-width: 520px;
}

.add-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}

/* 目录树对话框样式 */
.catalog-tree-dialog {
  max-height: 500px;
  overflow-y: auto;
}

.tree-node {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.node-type-tag {
  flex-shrink: 0;
}

.node-name {
  font-weight: 500;
}

.node-info {
  color: #909399;
  font-size: 12px;
}
</style>


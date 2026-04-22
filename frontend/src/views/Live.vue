<!--
  文件名: Live.vue
  功能: 摄像头管理页面（实时预览入口）
  描述:
    - 展示所有摄像头列表
    - 支持按名称、ID、所属平台搜索
    - 支持在线状态筛选
    - 单条删除、批量删除（同步后端删除目录设备节点）
    - 提供实时预览和回放入口（占位）
  依赖: Element Plus、Vue 3
-->
<template>
  <div class="page cameras">
    <header class="page-header">
      <div>
        <h2 class="page-title">摄像头管理</h2>
        <p class="page-subtitle">
          按所属平台统一管理摄像头列表，支持检索、过滤、实时预览与回看入口（占位）。
        </p>
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
            placeholder="按名称或 ID 搜索摄像头"
            @keyup.enter="onSearch"
            @clear="onResetFilters"
          />
          <el-input
            v-model="filterPlatform"
            clearable
            class="toolbar-select"
            placeholder="所属平台（名称或 ID）"
          />
          <el-select
            v-model="filterOnline"
            clearable
            placeholder="在线状态"
            class="toolbar-select"
            @change="onSearch"
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
            @change="onSearch"
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
            @change="onSearch"
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
          <div class="list-card-header">
            <span>摄像头列表</span>
            <el-button
              type="danger"
              plain
              :disabled="!selectedCameras.length"
              @click="onBatchDelete"
            >
              批量删除
            </el-button>
          </div>
        </template>
        <el-table
          ref="cameraTableRef"
          v-loading="loading"
          :data="pagedCameras"
          highlight-current-row
          style="width: 100%"
          @selection-change="onSelectionChange"
        >
          <el-table-column type="selection" width="48" />
          <el-table-column type="index" label="序号" width="50" :index="(index) => (currentPage - 1) * pageSize + index + 1" />
          <el-table-column prop="name" label="摄像头名称" min-width="140" show-overflow-tooltip />
          <el-table-column label="摄像头 ID" min-width="200" show-overflow-tooltip>
            <template #default="scope">
              <span :title="scope.row.deviceGbId || ''">{{ scope.row.deviceGbId || '—' }}</span>
            </template>
          </el-table-column>
          <el-table-column prop="platformId" label="所属平台ID" min-width="120" show-overflow-tooltip />
          <el-table-column prop="platformName" label="所属平台名称" min-width="140" show-overflow-tooltip />
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
          <el-table-column label="操作" min-width="200" fixed="right">
            <template #default="scope">
              <el-button
                size="small"
                type="primary"
                text
                @click="onPreview(scope.row)"
              >
                预览
              </el-button>
              <el-button
                size="small"
                text
                @click="goReplay(scope.row)"
              >
                回看
              </el-button>
              <el-button
                size="small"
                type="danger"
                text
                @click="onDeleteRow(scope.row)"
              >
                删除
              </el-button>
            </template>
          </el-table-column>
        </el-table>
        <div class="table-footer">
          <el-pagination
            v-model:current-page="currentPage"
            v-model:page-size="pageSize"
            :page-sizes="[10, 20, 50]"
            layout="total, sizes, prev, pager, next"
            :total="total"
            @current-change="loadCameras"
            @size-change="loadCameras"
          />
        </div>
      </el-card>
    </section>

    <el-dialog
      v-model="previewDialogVisible"
      :title="previewTitle"
      width="900px"
      top="5vh"
      @close="onPreviewDialogClose"
    >
      <div v-if="currentCamera">
        <!-- 播放器状态显示 -->
        <div v-if="previewState !== 'playing'" class="player-status-bar">
          <el-tag v-if="previewState === 'loading'" type="warning">正在建立连接...</el-tag>
          <el-tag v-else-if="previewState === 'error'" type="danger">{{ previewError }}</el-tag>
        </div>
        
        <!-- Jessibuca播放器容器 -->
        <div ref="playerContainer" class="player-container"></div>
        
        <!-- 播放器控制按钮 -->
        <div class="player-controls">
          <el-button 
            v-if="previewState === 'error'" 
            type="primary" 
            size="small"
            @click="restartPreview"
          >
            重新连接
          </el-button>
        </div>
        
        <PtzPanel
          :camera-id="currentCamera.id"
          :platform-gb-id="String(currentCamera.platformId || '')"
          :platform-db-id="currentCamera.platformDbId"
          :disabled="previewState !== 'playing'"
        />
      </div>
      <div v-else class="empty-tip">
        当前暂无选中摄像头。
      </div>
    </el-dialog>

  </div>
</template>

<script setup>
import { ref, computed, onMounted, watch, onUnmounted, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import PtzPanel from '@/components/PtzPanel.vue'
import { api } from '@/api/client'
import { createJessibucaPlayer, destroyJessibucaPlayer } from '@/composables/useJessibuca.js'

const router = useRouter()

const cameras = ref([])
const total = ref(0)
const loading = ref(false)

const searchInput = ref('')
const filterPlatform = ref('')
const filterOnline = ref('')
const filterListMode = ref('')
const filterUseMode = ref('')

const currentPage = ref(1)
const pageSize = ref(10)

const currentCamera = ref(null)

// 预览相关状态
const previewState = ref('idle') // idle/loading/playing/error
const previewError = ref('')
const playerContainer = ref(null)
/** @type {{ destroy?: function } | null} */
let playerHandle = null
let currentStreamId = null
const previewDialogVisible = ref(false)

const cameraTableRef = ref(null)
const selectedCameras = ref([])

const filteredCameras = computed(() => cameras.value)
const pagedCameras = computed(() => cameras.value)

async function loadCameras() {
  loading.value = true
  try {
    const params = {
      page: currentPage.value,
      pageSize: pageSize.value,
    }
    // 只添加有值的参数，避免发送 undefined
    const keyword = searchInput.value.trim()
    if (keyword) params.keyword = keyword
    
    const platformKeyword = filterPlatform.value.trim()
    if (platformKeyword) params.platformKeyword = platformKeyword
    
    if (filterOnline.value === 'online') params.online = 'true'
    else if (filterOnline.value === 'offline') params.online = 'false'

    const lt = filterListMode.value
    if (lt === 'normal' || lt === 'whitelist' || lt === 'blacklist') params.listType = lt

    const um = filterUseMode.value
    if (um === 'system') params.strategyMode = 'inherit'
    else if (um === 'independent') params.strategyMode = 'custom'

    const res = await api.listCameras(params)
    if (res.code === 0 && res.data) {
      cameras.value = (res.data.items || []).map((c) => {
        const sm = c.strategyMode === 'custom' ? 'independent' : 'system'
        return {
          id: c.id,
          deviceGbId: c.deviceGbId != null ? String(c.deviceGbId) : '',
          name: c.name,
          platformId: c.platformId,
          platformDbId: c.platformDbId,
          platformName: c.platformName || '',
          online: !!c.online,
          listMode: c.listType || 'normal',
          useMode: sm,
        }
      })
      total.value = res.data.total ?? 0
    }
  } catch (e) {
    ElMessage.error(e?.message || '加载摄像头列表失败')
    cameras.value = []
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
  loadCameras()
}

function onResetFilters() {
  searchInput.value = ''
  filterPlatform.value = ''
  filterOnline.value = ''
  filterListMode.value = ''
  filterUseMode.value = ''
  currentPage.value = 1
  loadCameras()
}

function onSelectionChange(rows) {
  selectedCameras.value = rows || []
}

async function onDeleteRow(row) {
  try {
    await ElMessageBox.confirm(
      `确定删除摄像头「${row.name || row.deviceGbId || row.id}」（国标 ${row.deviceGbId || '—'}）？将同步移除目录树中的对应设备节点；若平台再次同步目录，设备可能重新出现。`,
      '删除摄像头',
      { type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  try {
    const res = await api.deleteCamera(row.id)
    if (res.code !== 0) {
      ElMessage.error(res.message || '删除失败')
      return
    }
    ElMessage.success('已删除')
    if (currentCamera.value?.id === row.id) {
      previewDialogVisible.value = false
    }
    await loadCameras()
    cameraTableRef.value?.clearSelection?.()
    if (cameras.value.length === 0 && total.value > 0 && currentPage.value > 1) {
      currentPage.value -= 1
      await loadCameras()
    }
  } catch (e) {
    ElMessage.error(e?.message || '删除失败')
  }
}

async function onBatchDelete() {
  const rows = selectedCameras.value
  if (!rows.length) return
  try {
    await ElMessageBox.confirm(
      `确定删除选中的 ${rows.length} 路摄像头？将同步移除目录树中的对应设备节点。`,
      '批量删除',
      { type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  try {
    const ids = rows.map((r) => r.id)
    const res = await api.batchDeleteCameras(ids)
    if (res.code !== 0) {
      ElMessage.error(res.message || '批量删除失败')
      return
    }
    const d = res.data || {}
    if (currentCamera.value && ids.includes(currentCamera.value.id)) {
      previewDialogVisible.value = false
    }
    ElMessage.success(`已删除 ${d.deleted ?? 0} 条${d.notFound ? `，${d.notFound} 条不存在` : ''}`)
    cameraTableRef.value?.clearSelection?.()
    await loadCameras()
    if (cameras.value.length === 0 && total.value > 0 && currentPage.value > 1) {
      currentPage.value -= 1
      await loadCameras()
    }
  } catch (e) {
    ElMessage.error(e?.message || '批量删除失败')
  }
}

onMounted(loadCameras)
watch([currentPage, pageSize], () => { loadCameras() })

async function onPreview(row) {
  currentCamera.value = row
  previewDialogVisible.value = true
  previewState.value = 'loading'
  previewError.value = ''
  
  try {
    // 向后端发起点播请求，传入平台ID以确保精确定位摄像头
    const res = await api.startPreview(row.id, row.platformId, row.platformDbId)
    
    if (res.code !== 0) {
      previewState.value = 'error'
      previewError.value = res.message || '发起点播失败'
      ElMessage.error(res.message || '发起点播失败')
      return
    }
    
    // 保存流信息
    currentStreamId = res.data.streamId
    const flvUrl = res.data.flvUrl
    if (!flvUrl || String(flvUrl).trim() === '') {
      previewState.value = 'error'
      previewError.value = '未返回播放地址（flvUrl），请检查媒体配置、ZLM 是否可达'
      ElMessage.error(previewError.value)
      return
    }

    await nextTick()
    await nextTick()

    // 初始化Jessibuca播放器
    await initPlayer(flvUrl)
    
    previewState.value = 'playing'
    ElMessage.success('已开始预览')
    
  } catch (e) {
    previewState.value = 'error'
    previewError.value = e.message || '连接失败'
    ElMessage.error(e.message || '连接失败')
  }
}

async function initPlayer(flvUrl) {
  if (!playerContainer.value) {
    throw new Error('播放器容器未准备好')
  }

  destroyPlayer()

  try {
    playerHandle = await createJessibucaPlayer(playerContainer.value, flvUrl, {
      onError: (err) => {
        console.error('Player error:', err)
        previewState.value = 'error'
        previewError.value = `播放错误: ${String(err?.message || err)}`
      },
    })
  } catch (error) {
    console.error('Jessibuca init failed:', error)
    const detail =
      (error && (error.message || (typeof error.toString === 'function' && error.toString()))) ||
      String(error)
    throw new Error(detail && detail !== '[object Object]' ? detail : '播放流失败')
  }
}

function destroyPlayer() {
  if (playerHandle) {
    destroyJessibucaPlayer(playerHandle)
    playerHandle = null
  }
}

function resetPreviewUiState() {
  destroyPlayer()
  previewState.value = 'idle'
  previewError.value = ''
  currentStreamId = null
  currentCamera.value = null
}

async function restartPreview() {
  if (!currentCamera.value) return
  await onPreview(currentCamera.value)
}

function onPreviewDialogClose() {
  resetPreviewUiState()
}

function goReplay(row) {
  router.push({
    path: '/live/replay',
    query: {
      cameraId: row.id,
      cameraName: row.name,
      platformGbId: String(row.platformId || '').trim(),
      platformDbId: row.platformDbId != null && row.platformDbId !== '' ? String(row.platformDbId) : '',
    },
  })
}

const previewTitle = computed(() => {
  if (!currentCamera.value) return '实时预览'
  return `实时预览 - ${currentCamera.value.name} / ${currentCamera.value.deviceGbId || currentCamera.value.id}`
  })
  
  // 组件卸载时清理
onUnmounted(() => {
  destroyPlayer()
})
</script>

<style scoped>
/* 主内容区与实时预览墙、系统配置一致：铺满 app-main，不限制 max-width */


.page-header {
  margin-bottom: 16px;
}

.list-card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
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

.player-wrap {
  border: 1px dashed #dcdfe6;
  border-radius: 4px;
  height: 360px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: #1f2d3d;
  margin-top: 12px;
}

.player-placeholder {
  color: #c0c4cc;
  font-size: 14px;
  text-align: center;
  padding: 0 40px;
}

/* Jessibuca：由 player.resize() 同步 canvas 与容器；此处只固定容器占位，避免 !important 与内部尺寸计算冲突导致画面缩在角落 */
.player-container {
  width: 100%;
  height: 480px;
  background: #000;
  border-radius: 4px;
  overflow: hidden;
  position: relative;
}

.player-container :deep(canvas),
.player-container :deep(video) {
  display: block;
  vertical-align: top;
}

.player-status-bar {
  margin-bottom: 8px;
  text-align: center;
}

.player-controls {
  margin-top: 12px;
  margin-bottom: 12px;
  text-align: center;
}

.dialog-desc {
  font-size: 13px;
  color: var(--gb-text-soft);
  margin-bottom: 8px;
}

.empty-tip {
  padding: 40px 0;
  text-align: center;
  color: #909399;
}
</style>


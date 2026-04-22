<template>
  <div class="page alarms">
    <header class="page-header">
      <div>
        <h2 class="page-title">告警列表</h2>
        <p class="page-subtitle">
          查看并筛选来自设备与平台的国标告警事件。
        </p>
      </div>
    </header>
    <el-card shadow="hover" class="search-card">
      <template #header>
        <span>筛选条件</span>
      </template>
      <el-form :model="query" label-width="80px" inline>
        <el-form-item label="级别">
          <el-select v-model="query.level" placeholder="全部">
            <el-option label="全部" value="" />
            <el-option label="一般" value="info" />
            <el-option label="重要" value="major" />
            <el-option label="紧急" value="critical" />
          </el-select>
        </el-form-item>
        <el-form-item label="状态">
          <el-select v-model="query.status" placeholder="全部">
            <el-option label="全部" value="" />
            <el-option label="未确认" value="new" />
            <el-option label="已确认" value="ack" />
            <el-option label="已处置" value="disposed" />
          </el-select>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" @click="onSearch">查询</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card shadow="hover">
      <template #header>
        <div class="header-row">
          <span>告警列表</span>
          <el-button size="small" @click="onMockReport">模拟上报告警</el-button>
        </div>
      </template>
      <el-table v-loading="loading" :data="rows" style="width: 100%">
        <el-table-column prop="occurredAt" label="时间" width="180" />
        <el-table-column prop="channelName" label="通道" width="200" />
        <el-table-column label="级别" width="100">
          <template #default="{ row }">
            <el-tag :type="getLevelType(row.level)" size="small">{{ getLevelLabel(row.level) }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="状态" width="100">
          <template #default="{ row }">
            {{ getStatusLabel(row.status) }}
          </template>
        </el-table-column>
        <el-table-column prop="description" label="描述" />
        <el-table-column label="操作" width="180" fixed="right">
          <template #default="{ row }">
            <el-button
              v-if="row.status === 'new'"
              type="primary"
              size="small"
              text
              @click="onConfirm(row)"
            >
              确认
            </el-button>
            <el-button
              v-if="row.status === 'new' || row.status === 'ack'"
              type="warning"
              size="small"
              text
              @click="onDispose(row)"
            >
              处置
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
          @current-change="loadAlarms"
          @size-change="loadAlarms"
        />
      </div>
    </el-card>
    <el-dialog v-model="disposeDialogVisible" title="告警处置" width="440px">
      <el-input
        v-model="disposeNote"
        type="textarea"
        :rows="3"
        placeholder="处置说明（可选）"
      />
      <template #footer>
        <el-button @click="disposeDialogVisible = false">取消</el-button>
        <el-button type="primary" @click="submitDispose" :loading="disposeSaving">确定</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { reactive, ref, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import { api } from '@/api/client'

const query = reactive({
  level: '',
  status: '',
})
const rows = ref([])
const total = ref(0)
const loading = ref(false)
const currentPage = ref(1)
const pageSize = ref(10)

const disposeDialogVisible = ref(false)
const disposeNote = ref('')
const disposeSaving = ref(false)
const disposingRow = ref(null)

function getLevelLabel(level) {
  if (level === 'critical') return '紧急'
  if (level === 'major') return '重要'
  return '一般'
}
function getLevelType(level) {
  if (level === 'critical') return 'danger'
  if (level === 'major') return 'warning'
  return 'info'
}
function getStatusLabel(status) {
  if (status === 'ack') return '已确认'
  if (status === 'disposed') return '已处置'
  return '未确认'
}

async function loadAlarms() {
  loading.value = true
  try {
    const params = {
      page: currentPage.value,
      pageSize: pageSize.value,
      level: query.level || undefined,
      status: query.status || undefined,
    }
    const res = await api.listAlarms(params)
    if (res?.code === 0 && res?.data) {
      rows.value = res.data.items || []
      total.value = res.data.total ?? 0
    }
  } catch (e) {
    ElMessage.error(e?.message || '加载告警列表失败')
    rows.value = []
    total.value = 0
  } finally {
    loading.value = false
  }
}

function onSearch() {
  currentPage.value = 1
  loadAlarms()
}

async function onMockReport() {
  try {
    await api.postAlarm({
      channelId: 'mock-1',
      channelName: '测试通道',
      level: 'major',
      description: '模拟区域入侵告警（测试）',
    })
    ElMessage.success('已上报一条模拟告警')
    await loadAlarms()
  } catch (e) {
    ElMessage.error(e?.message || '上报失败')
  }
}

async function onConfirm(row) {
  try {
    await api.putAlarm(row.id, { status: 'ack' })
    ElMessage.success('已确认')
    await loadAlarms()
  } catch (e) {
    ElMessage.error(e?.message || '确认失败')
  }
}

function onDispose(row) {
  disposingRow.value = row
  disposeNote.value = ''
  disposeDialogVisible.value = true
}

async function submitDispose() {
  if (!disposingRow.value) return
  disposeSaving.value = true
  try {
    await api.putAlarm(disposingRow.value.id, { status: 'disposed', disposeNote: disposeNote.value })
    ElMessage.success('已处置')
    disposeDialogVisible.value = false
    await loadAlarms()
  } catch (e) {
    ElMessage.error(e?.message || '处置失败')
  } finally {
    disposeSaving.value = false
  }
}

onMounted(loadAlarms)
</script>

<style scoped>
/* 主内容区与实时预览墙、系统配置一致：铺满 app-main，不限制 max-width */


.page-header {
  margin-bottom: 16px;
}

.search-card {
  margin-bottom: 16px;
}

.table-footer {
  margin-top: 12px;
  display: flex;
  justify-content: flex-end;
}

.header-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
}
</style>


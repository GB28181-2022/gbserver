<template>
  <div class="home page">
    <header class="page-header">
      <div>
        <h2 class="page-title">概览</h2>
        <p class="page-subtitle">当前部署状态与核心运行视图</p>
      </div>
    </header>

    <section class="page-section">
      <el-row :gutter="16">
        <el-col :span="8">
          <el-card class="metric-card" shadow="hover">
            <div class="metric-label">后端健康</div>
            <div class="metric-value">
              <span
                class="status-dot"
                :class="statusDotClass"
              />
              <span>{{ healthLabel }}</span>
            </div>
            <p class="metric-desc">
              {{ healthDesc }}
            </p>
          </el-card>
        </el-col>

        <el-col :span="8">
          <el-card class="metric-card" shadow="hover">
            <div class="metric-label">下级平台 / 摄像头</div>
            <div class="metric-value">
              <span v-if="overview">{{ overview.devicePlatformOnline }} / {{ overview.devicePlatformTotal }} 平台</span>
              <span v-else class="metric-muted">--</span>
            </div>
            <p class="metric-desc">
              <template v-if="overview">摄像头 {{ overview.cameraOnline }} / {{ overview.cameraTotal }} 在线。</template>
              <template v-else>来自概览接口。</template>
            </p>
          </el-card>
        </el-col>

        <el-col :span="8">
          <el-card class="metric-card" shadow="hover">
            <div class="metric-label">未确认告警 / 上级平台</div>
            <div class="metric-value">
              <span v-if="overview">{{ overview.alarmNewCount }} 条告警</span>
              <span v-else class="metric-muted">--</span>
            </div>
            <p class="metric-desc">
              <template v-if="overview">上级平台 {{ overview.upstreamPlatformTotal }} 个。</template>
              <template v-else>来自概览接口。</template>
            </p>
          </el-card>
        </el-col>
      </el-row>
    </section>

    <section class="page-section">
      <el-card class="status-card" shadow="hover">
        <template #header>
          <span>服务详情</span>
        </template>
        <div class="status-body">
          <div class="status-main-line">
            <span class="status-key">健康检查</span>
            <span class="status-value">
              <span
                class="status-dot"
                :class="statusDotClass"
              />
              {{ healthLabel }}
            </span>
          </div>
          <p class="status-note">
            通过 <code>/api/health</code> 获取当前后端健康状态，仅用于快速连通性确认。
          </p>
        </div>
      </el-card>
    </section>
  </div>
</template>

<script setup>
import { ref, onMounted, computed } from 'vue'
import { api } from '@/api/client'

const health = ref('检查中…')
const overview = ref(null)

async function checkHealth(url) {
  const r = await fetch(url)
  const text = await r.text()
  if (!r.ok) return null
  try {
    const j = JSON.parse(text)
    return j.status || 'ok'
  } catch {
    return null
  }
}

onMounted(async () => {
  const origin = window.location.origin
  const url = `${origin}/api/health`
  try {
    let result = await checkHealth(url)
    if (result == null && origin.includes('192.168.1.9')) {
      result = await checkHealth('http://127.0.0.1/api/health')
    }
    health.value = result != null ? result : '不可达'
  } catch (e) {
    health.value = (e && e.message) ? e.message : String(e)
  }
  try {
    const res = await api.getOverview()
    if (res?.code === 0 && res?.data) overview.value = res.data
  } catch {
    overview.value = null
  }
})

const isHealthy = computed(() => health.value === 'ok')

const healthLabel = computed(() =>
  isHealthy.value ? '运行正常' : '状态异常'
)

const healthDesc = computed(() =>
  isHealthy.value
    ? '所有核心服务已启动，可进行后续平台接入与设备联调。'
    : `当前状态：${health.value}，请检查后端服务是否启动或健康。`
)

const statusDotClass = computed(() =>
  isHealthy.value ? 'status-dot-ok' : 'status-dot-warn'
)
</script>

<style scoped>
.page-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-end;
  margin-bottom: 18px;
}

.metric-card {
  min-height: 140px;
}

.metric-label {
  font-size: 12px;
  letter-spacing: 0.16em;
  text-transform: uppercase;
  color: var(--gb-text-soft);
  margin-bottom: 8px;
}

.metric-value {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 18px;
  margin-bottom: 4px;
}

.metric-muted {
  color: var(--gb-text-soft);
}

.metric-desc {
  margin: 0;
  font-size: 12px;
  color: var(--gb-text-soft);
}

.status-card {
  margin-top: 4px;
}

.status-body {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.status-main-line {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 13px;
}

.status-key {
  color: var(--gb-text-soft);
}

.status-value {
  display: inline-flex;
  align-items: center;
  gap: 8px;
}

.status-dot {
  width: 9px;
  height: 9px;
  border-radius: 999px;
}

.status-dot-ok {
  background: var(--gb-success);
  box-shadow: 0 0 10px rgba(34, 197, 94, 0.9);
}

.status-dot-warn {
  background: var(--gb-warning);
  box-shadow: 0 0 10px rgba(234, 179, 8, 0.9);
}

.status-note {
  margin: 0;
  font-size: 12px;
  color: var(--gb-text-soft);
}

code {
  padding: 1px 5px;
  border-radius: 4px;
  background: rgba(15, 23, 42, 0.9);
  border: 1px solid rgba(148, 163, 184, 0.4);
  font-size: 12px;
}
</style>

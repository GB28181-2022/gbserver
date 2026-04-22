<!--
  @file PtzPanel.vue
  @brief 国标云台控制条：方向、变焦、光圈、全停、速度
  @details 调用 api.postPtz → POST /api/ptz；需 cameraId 与 platformGbId（与预览 start 一致）。
  @dependency Vue 3、Element Plus、@/api/client
  @date 2025
  @note 方向/变焦/光圈仅单击步进：先发 start，约 PTZ_STEP_PULSE_MS 后自动 stop；不按 mousedown 长按触发
-->
<template>
  <div class="ptz-panel" :class="{ 'is-disabled': finalDisabled }">
    <p v-if="finalDisabled && hintText" class="ptz-tip">{{ hintText }}</p>
    <template v-else-if="!finalDisabled">
      <div class="ptz-row ptz-row-main">
        <div class="ptz-cross-horizontal">
          <el-button
            size="small"
            :disabled="finalDisabled"
            aria-label="云台左"
            title="左（单击步进）"
            @click="onClickStep('left')"
          >
            左
          </el-button>
          <div class="ptz-cross-vertical">
            <el-button
              size="small"
              :disabled="finalDisabled"
              aria-label="云台上"
              title="上（单击步进）"
              @click="onClickStep('up')"
            >
              上
            </el-button>
            <el-button
              size="small"
              type="warning"
              :disabled="finalDisabled"
              aria-label="停止"
              title="停止"
              @click="onStopAll"
            >
              停
            </el-button>
            <el-button
              size="small"
              :disabled="finalDisabled"
              aria-label="云台下"
              title="下（单击步进）"
              @click="onClickStep('down')"
            >
              下
            </el-button>
          </div>
          <el-button
            size="small"
            :disabled="finalDisabled"
            aria-label="云台右"
            title="右（单击步进）"
            @click="onClickStep('right')"
          >
            右
          </el-button>
        </div>
        <div class="ptz-zoom-iris">
          <div class="ptz-group">
            <span class="ptz-label">变焦</span>
            <el-button-group>
              <el-button size="small" title="放大（单击步进）" @click="onClickStep('zoomIn')">放大</el-button>
              <el-button size="small" title="缩小（单击步进）" @click="onClickStep('zoomOut')">缩小</el-button>
            </el-button-group>
          </div>
          <div class="ptz-group">
            <span class="ptz-label">光圈</span>
            <el-button-group>
              <el-button size="small" title="光圈+（单击步进）" @click="onClickStep('irisOpen')">+</el-button>
              <el-button size="small" title="光圈−（单击步进）" @click="onClickStep('irisClose')">−</el-button>
            </el-button-group>
          </div>
        </div>
        <div class="ptz-meta">
          <el-button size="small" type="danger" plain :disabled="finalDisabled" @click="onStopAll">全部停止</el-button>
          <div class="ptz-speed">
            <span class="ptz-label">速度</span>
            <el-select v-model="speed" size="small" class="ptz-speed-select">
              <el-option label="低" value="1" />
              <el-option label="中" value="2" />
              <el-option label="高" value="3" />
            </el-select>
          </div>
        </div>
      </div>
    </template>
  </div>
</template>

<script setup>
import { ref, computed, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { api } from '@/api/client'

/** 单击步进：start 后延迟再 stop（毫秒），避免长按/滑过误触 */
const PTZ_STEP_PULSE_MS = 400

const props = defineProps({
  cameraId: { type: String, default: '' },
  /** 平台国标 ID，与 preview/start 的 platformGbId 一致 */
  platformGbId: { type: String, default: '' },
  platformDbId: { type: [String, Number], default: '' },
  disabled: { type: Boolean, default: false },
})

const speed = ref('2')

const commandMap = {
  up: 'up',
  down: 'down',
  left: 'left',
  right: 'right',
  zoomIn: 'zoomIn',
  zoomOut: 'zoomOut',
  irisOpen: 'irisOpen',
  irisClose: 'irisClose',
}

const finalDisabled = computed(
  () => props.disabled || !props.cameraId || !String(props.platformGbId || '').trim(),
)

const hintText = computed(() => {
  if (props.disabled) return '当前不可操作云台'
  if (!props.cameraId) return '请先选择摄像机通道'
  if (!String(props.platformGbId || '').trim()) return '缺少平台国标 ID，无法下发云台'
  return ''
})

/**
 * 调用后端云台接口
 * @param {string} command up/down/left/right/zoomIn/zoomOut/irisOpen/irisClose/stop
 * @param {string} action start 或 stop；全停为 command=stop & action=stop
 * @param {{ showSuccess?: boolean }} opts 是否弹出成功提示（步进中间帧通常关闭）
 * @returns {Promise<boolean>} HTTP+code===0 为 true
 */
async function sendPtz(command, action, { showSuccess = true } = {}) {
  if (finalDisabled.value) return false
  try {
    const res = await api.postPtz({
      cameraId: props.cameraId,
      platformGbId: props.platformGbId,
      platformDbId: props.platformDbId === '' || props.platformDbId == null ? undefined : props.platformDbId,
      command,
      action,
      speed: parseInt(speed.value, 10) || 2,
    })
    if (res.code !== 0) {
      ElMessage.error(res.message || '云台指令失败')
      return false
    }
    if (showSuccess) ElMessage.success('云台指令已发送')
    return true
  } catch (e) {
    ElMessage.error(e?.message || '云台请求失败')
    return false
  }
}

/** @type {ReturnType<typeof setTimeout> | null} */
let stepPulseTimer = null

/**
 * 单击步进：先 start，再在 PTZ_STEP_PULSE_MS 后 stop；并发点击会重置定时器
 * @param {string} cmdKey UI 键名，经 commandMap 映射为 API command
 */
function onClickStep(cmdKey) {
  if (finalDisabled.value) return
  const cmd = commandMap[cmdKey] || cmdKey
  if (stepPulseTimer != null) {
    clearTimeout(stepPulseTimer)
    stepPulseTimer = null
  }
  void (async () => {
    const okStart = await sendPtz(cmd, 'start', { showSuccess: false })
    if (!okStart) return
    stepPulseTimer = setTimeout(() => {
      stepPulseTimer = null
      void (async () => {
        const okStop = await sendPtz(cmd, 'stop', { showSuccess: false })
        if (okStop) ElMessage.success('云台步进指令已发送')
        else ElMessage.warning('步进停止指令未确认成功，可点「停」或「全部停止」')
      })()
    }, PTZ_STEP_PULSE_MS)
  })()
}

/** 发送全停并清除未完成的步进定时器 */
function onStopAll() {
  if (finalDisabled.value) return
  if (stepPulseTimer != null) {
    clearTimeout(stepPulseTimer)
    stepPulseTimer = null
  }
  sendPtz('stop', 'stop')
}

onUnmounted(() => {
  if (stepPulseTimer != null) {
    clearTimeout(stepPulseTimer)
    stepPulseTimer = null
  }
})
</script>

<style scoped>
.ptz-panel {
  margin-top: var(--gb-spacing-md, 16px);
  padding: var(--gb-spacing-md, 16px);
  background: var(--gb-bg-soft, #e5e7eb);
  border-radius: var(--gb-radius-md, 8px);
  min-height: 72px;
}

.ptz-panel.is-disabled {
  opacity: 0.75;
}

.ptz-panel.is-disabled .ptz-tip {
  margin: 0;
}

.ptz-tip {
  margin: 0;
  font-size: 13px;
  color: var(--gb-text-muted, #6b7280);
}

.ptz-row-main {
  display: flex;
  align-items: center;
  gap: var(--gb-spacing-lg, 24px);
  flex-wrap: wrap;
}

.ptz-cross-horizontal {
  display: flex;
  flex-direction: row;
  align-items: center;
  gap: 4px;
}

.ptz-cross-vertical {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
}

.ptz-zoom-iris {
  display: flex;
  align-items: center;
  gap: var(--gb-spacing-md, 16px);
}

.ptz-group {
  display: flex;
  align-items: center;
  gap: var(--gb-spacing-xs, 4px);
}

.ptz-label {
  font-size: 12px;
  color: var(--gb-text-muted, #6b7280);
  margin-right: 4px;
}

.ptz-meta {
  display: flex;
  align-items: center;
  gap: var(--gb-spacing-md, 16px);
  margin-left: auto;
}

.ptz-speed {
  display: flex;
  align-items: center;
  gap: 4px;
}

.ptz-speed-select {
  width: 72px;
}
</style>

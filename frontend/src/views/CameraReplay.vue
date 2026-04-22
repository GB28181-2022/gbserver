<template>
  <div class="page camera-replay">
    <header class="page-header replay-header">
      <div class="replay-header-left">
        <el-button class="back-btn" @click="goBack">返回</el-button>
        <h2 class="page-title replay-page-title">录像检索与回放</h2>
      </div>
    </header>

    <section class="page-section">
      <el-card shadow="hover">
        <template #header>
          <span>检索条件</span>
        </template>
        <div class="replay-search">
          <el-descriptions :column="2" border size="small" class="replay-camera-info">
            <el-descriptions-item label="摄像头名称">{{ cameraName }}</el-descriptions-item>
            <el-descriptions-item label="摄像头 ID">{{ cameraId }}</el-descriptions-item>
            <el-descriptions-item v-if="platformGbId" label="平台国标 ID">{{ platformGbId }}</el-descriptions-item>
            <el-descriptions-item v-if="routePlatformId" label="平台库 ID">{{ routePlatformId }}</el-descriptions-item>
          </el-descriptions>
          <div class="replay-time-row">
            <el-date-picker
              v-model="replayTimeRange"
              type="datetimerange"
              range-separator="至"
              start-placeholder="开始时间"
              end-placeholder="结束时间"
              value-format="YYYY-MM-DD HH:mm:ss"
              :shortcuts="replayTimeShortcuts"
              class="replay-time-picker"
            />
            <el-button type="primary" :loading="replaySearching" @click="onReplaySearch">
              检索
            </el-button>
            <el-button v-if="isDev" type="success" :loading="replaySearching" @click="onReplaySearchLast1h">
              用最近1小时检索
            </el-button>
            <el-button @click="onReplaySearchReset">重置</el-button>
            <el-button type="danger" plain :loading="clearingData" @click="onClearCameraData">
              清空本机数据
            </el-button>
          </div>
        </div>
      </el-card>
    </section>

    <section class="page-section">
      <el-card shadow="hover">
        <template #header>
          <span>录像列表</span>
        </template>
        <el-table
          :data="pagedReplayList"
          height="320"
          size="small"
          highlight-current-row
          class="replay-table"
        >
          <el-table-column prop="startTime" label="开始时间" width="170" />
          <el-table-column prop="endTime" label="结束时间" width="170" />
          <el-table-column prop="durationLabel" label="时长" width="90" />
          <el-table-column prop="streamType" label="码流" width="80" />
          <el-table-column label="操作" width="160" fixed="right">
            <template #default="scope">
              <el-button size="small" type="primary" text @click="onPreview(scope.row)">
                预览
              </el-button>
              <el-button size="small" text @click="onDownload(scope.row)">
                下载
              </el-button>
            </template>
          </el-table-column>
        </el-table>
        <div v-if="!replaySearchDone" class="replay-list-tip">
          请选择开始、结束时间后点击「检索」，后端将按国标 RecordInfo 向平台查询录像并返回列表。
        </div>
        <div v-else-if="replayList.length === 0" class="replay-list-tip">
          该时间段内暂无录像。
        </div>
        <div v-else class="replay-list-footer">
          <el-pagination
            v-model:current-page="replayPage"
            v-model:page-size="replayPageSize"
            :page-sizes="[10, 20]"
            :total="replayTotal"
            layout="total, sizes, prev, pager, next"
            small
          />
        </div>
      </el-card>
    </section>

    <el-dialog
      v-model="previewDialogVisible"
      :title="previewDialogTitle"
      width="800px"
      top="8vh"
      @closed="onPreviewDialogClosed"
    >
      <div v-if="previewRecord">
        <div v-if="previewState !== 'playing'" class="player-status-bar">
          <el-tag v-if="previewState === 'loading'" type="warning">正在建立回放连接…</el-tag>
          <el-tag v-else-if="previewState === 'error'" type="danger">{{ previewError }}</el-tag>
        </div>
        <div ref="playerContainer" class="player-container"></div>
        <div
          v-if="previewRecord && previewState === 'playing' && replaySegmentDurationSec > 0"
          class="replay-timeline-bar"
        >
          <div class="replay-timeline-row">
            <el-slider
              :model-value="replaySliderDisplaySec"
              :min="0"
              :max="replaySliderMaxSec"
              :disabled="replaySliderMaxSec <= 0"
              :show-tooltip="false"
              @input="onReplayTimelineInput"
              @change="onReplayTimelineChange"
            />
          </div>
          <div class="replay-timeline-meta">
            <span>{{ formatReplayWallClock((previewRecord.startTimeUnix || 0) + Math.floor(replaySliderDisplaySec)) }}</span>
            <span class="replay-timeline-sep">/</span>
            <span>{{ formatReplayWallClock(previewRecord.endTimeUnix) }}</span>
          </div>
          <div class="replay-rate-row">
            <span class="replay-rate-label">倍速</span>
            <el-select
              v-model="replayPlaybackRate"
              size="small"
              class="replay-rate-select"
              @change="onReplayPlaybackRateChange"
            >
              <el-option
                v-for="r in replayRateOptions"
                :key="r"
                :label="`${r}x`"
                :value="r"
                :class="{ 'rate-fast': r >= 4, 'rate-slow': r < 1 }"
              />
            </el-select>
          </div>
        </div>
        <div v-if="previewState === 'error'" class="player-controls">
          <el-button type="primary" size="small" @click="retryPreview">重试</el-button>
        </div>
        <PtzPanel
          :camera-id="cameraId"
          :platform-gb-id="platformGbId"
          :platform-db-id="platformDbIdNum"
          disabled
        />
      </div>
    </el-dialog>

    <el-dialog
      v-model="downloadDialogVisible"
      title="录像下载"
      width="420px"
      :close-on-click-modal="false"
      @closed="onDownloadDialogClosed"
    >
      <div v-if="downloadRecord">
        <p class="download-desc">{{ downloadRecord.startTime }}～{{ downloadRecord.endTime }}</p>
        <el-progress
          :percentage="downloadProgress"
          :status="downloadStatus === 'done' ? 'success' : downloadStatus === 'error' || downloadStatus === 'cancelled' ? 'exception' : undefined"
        />
        <p class="download-status">
          {{
            downloadStatus === 'downloading' && downloadProgress < 10
              ? '正在连接设备…'
              : downloadStatus === 'downloading' && downloadProgress >= 10 && downloadProgress < 90
                ? `高速下载中 (${downloadProgress}%)…`
                : downloadStatus === 'downloading' && downloadProgress >= 90
                  ? '正在生成文件…'
                  : downloadStatus === 'done'
                    ? '下载完成'
                    : downloadStatus === 'cancelled'
                      ? '已取消'
                      : downloadStatus === 'error'
                        ? '下载失败'
                        : ''
          }}
        </p>
      </div>
      <template #footer>
        <el-button v-if="downloadStatus === 'downloading'" @click="cancelCurrentDownload">
          取消下载
        </el-button>
        <el-button
          v-if="downloadStatus === 'done' || downloadStatus === 'error' || downloadStatus === 'cancelled'"
          type="primary"
          @click="downloadDialogVisible = false"
        >
          关闭
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import PtzPanel from '@/components/PtzPanel.vue'
import { api } from '@/api/client'
import { createJessibucaPlayer, destroyJessibucaPlayer } from '@/composables/useJessibuca.js'

const route = useRoute()
const router = useRouter()

const isDev = import.meta.env.DEV
const cameraId = computed(() => String(route.query.cameraId || ''))
const cameraName = computed(() => route.query.cameraName || '')
/** 与列表页一致：下级平台国标 ID */
const platformGbId = computed(() => String(route.query.platformGbId || '').trim())
/** 目录挂载等场景仅带库表 platform_id */
const routePlatformId = computed(() => String(route.query.platformId || '').trim())
const platformDbIdStr = computed(() => String(route.query.platformDbId || '').trim())
const platformDbIdNum = computed(() => {
  const s = platformDbIdStr.value
  if (!s) return undefined
  const n = Number(s)
  return Number.isFinite(n) ? n : undefined
})

const replayTimeRange = ref(null)
const replayTimeShortcuts = [
  {
    text: '最近1小时',
    value: () => {
      const end = new Date()
      const start = new Date(end.getTime() - 3600 * 1000)
      return [start, end]
    },
  },
  {
    text: '今天',
    value: () => {
      const end = new Date()
      const start = new Date(end.getFullYear(), end.getMonth(), end.getDate())
      return [start, end]
    },
  },
  {
    text: '昨天',
    value: () => {
      const d = new Date()
      d.setDate(d.getDate() - 1)
      const start = new Date(d.getFullYear(), d.getMonth(), d.getDate())
      const end = new Date(start.getTime() + 24 * 3600 * 1000 - 1)
      return [start, end]
    },
  },
]
const replaySearching = ref(false)
const clearingData = ref(false)
const replaySearchDone = ref(false)
const replayList = ref([])
const replayPage = ref(1)
const replayPageSize = ref(10)
const replayTotal = ref(0)

const pagedReplayList = computed(() => {
  const start = (replayPage.value - 1) * replayPageSize.value
  return replayList.value.slice(start, start + replayPageSize.value)
})

const previewDialogVisible = ref(false)
const previewRecord = ref(null)
const previewState = ref('idle')
const previewError = ref('')
const playerContainer = ref(null)
/** @type {{ destroy?: function, setPlaybackRate?: function } | null} */
let playerHandle = null
const replayStreamId = ref(null)

/** 回放：当前 INVITE 时间窗（Unix 秒）与墙钟起点，用于进度条估算 */
const replaySessionStartUnix = ref(0)
const replaySessionEndUnix = ref(0)
const replayPlayWallStartMs = ref(0)
const replayPlaybackRate = ref(1)
const replayUiTick = ref(0)
/** 拖动时间条时暂停进度覆盖 */
const replayTimelineDragging = ref(false)
const replayTimelineDragValue = ref(0)
let replayProgressTimer = null

const previewDialogTitle = computed(() => {
  if (!previewRecord.value) return '预览'
  return `回放预览 - ${previewRecord.value.startTime}～${previewRecord.value.endTime}`
})

const replayRateOptions = [0.5, 1, 1.25, 1.5, 2, 4, 8, 16, 32]

const replaySegmentDurationSec = computed(() => {
  const r = previewRecord.value
  if (!r || !r.endTimeUnix || !r.startTimeUnix) return 0
  return Math.max(0, r.endTimeUnix - r.startTimeUnix)
})

/** 可拖动上限：保证距片尾至少 10 秒（与后端最短窗口一致） */
const replaySliderMaxSec = computed(() => Math.max(0, replaySegmentDurationSec.value - 10))

/** 估算当前播放到录像时间轴上的 Unix 秒（片段内） */
const replayProgressUnix = computed(() => {
  replayUiTick.value
  const r = previewRecord.value
  if (!r?.startTimeUnix || !replaySessionStartUnix.value || !replayPlayWallStartMs.value) {
    return r?.startTimeUnix ?? 0
  }
  const segStart = r.startTimeUnix
  const segEnd = r.endTimeUnix
  const elapsed = (Date.now() - replayPlayWallStartMs.value) / 1000
  const at = replaySessionStartUnix.value + elapsed * replayPlaybackRate.value
  return Math.min(segEnd, Math.max(segStart, at))
})

const replaySliderDisplaySec = computed(() => {
  if (replayTimelineDragging.value) return replayTimelineDragValue.value
  const r = previewRecord.value
  if (!r?.startTimeUnix) return 0
  return Math.max(0, replayProgressUnix.value - r.startTimeUnix)
})

/**
 * 与后端 `to_char(... AT TIME ZONE 'Asia/Shanghai')` 一致：Unix 秒 → 上海墙钟 YYYY-MM-DD HH:mm:ss。
 * 避免用 Date#getHours()（浏览器本地时区）导致拖动条提示与列表「开始/结束」列相差整时区。
 */
function formatReplayWallClock(ts) {
  if (ts == null || !Number.isFinite(Number(ts)) || Number(ts) <= 0) return '—'
  const d = new Date(Math.floor(Number(ts)) * 1000)
  const parts = new Intl.DateTimeFormat('en-CA', {
    timeZone: 'Asia/Shanghai',
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  }).formatToParts(d)
  const g = (t) => parts.find((p) => p.type === t)?.value ?? ''
  return `${g('year')}-${g('month')}-${g('day')} ${g('hour')}:${g('minute')}:${g('second')}`
}

function stopReplayProgressTimer() {
  if (replayProgressTimer != null) {
    clearInterval(replayProgressTimer)
    replayProgressTimer = null
  }
}

function startReplayProgressTimer() {
  stopReplayProgressTimer()
  replayProgressTimer = setInterval(() => {
    replayUiTick.value++
  }, 400)
}

/**
 * 通过后端 SIP INFO + MANSRTSP 通知设备端改变回放发送速率。
 * HTTP-FLV 是"直播式"推流，浏览器端 video.playbackRate 无效；
 * 必须让设备按指定倍速发送 RTP 数据。
 */
async function applyReplayPlaybackRate() {
  const rate = replayPlaybackRate.value
  if (!replayStreamId.value || !cameraId.value) return
  try {
    const res = await api.setReplaySpeed(cameraId.value, {
      streamId: replayStreamId.value,
      scale: rate,
    })
    if (res.code === 0) {
      console.log('[PlaybackRate] SIP INFO 倍速指令已发送:', rate)
    } else {
      console.warn('[PlaybackRate] 倍速设置失败:', res.message)
      ElMessage.warning(`倍速设置失败: ${res.message || '未知错误'}`)
    }
  } catch (e) {
    console.error('[PlaybackRate] 倍速 API 调用异常:', e)
    ElMessage.error(`倍速设置异常: ${e?.message || '网络错误'}`)
  }
}

function onReplayPlaybackRateChange() {
  const rate = replayPlaybackRate.value
  console.log('[PlaybackRate] 用户选择倍速:', rate)
  if (previewState.value === 'playing' && replayStreamId.value) {
    applyReplayPlaybackRate()
  }
}

function onReplayTimelineInput(val) {
  replayTimelineDragging.value = true
  replayTimelineDragValue.value = val
}

async function onReplayTimelineChange(val) {
  replayTimelineDragging.value = false
  if (previewState.value !== 'playing' && previewState.value !== 'loading') return
  await seekReplayToSliderSec(val)
}

async function seekReplayToSliderSec(sliderSec) {
  const row = previewRecord.value
  if (!row?.segmentDbId || !row.startTimeUnix || !row.endTimeUnix) return
  const targetUnix = row.startTimeUnix + Math.floor(sliderSec)
  if (row.endTimeUnix - targetUnix < 10) {
    ElMessage.warning('距离片尾不足 10 秒，请向前拖动')
    return
  }
  previewState.value = 'loading'
  previewError.value = ''
  try {
    if (replayStreamId.value) {
      await api.stopReplay(cameraId.value, { streamId: replayStreamId.value })
    }
    destroyPlayer()
    replayStreamId.value = null
    stopReplayProgressTimer()
    const body = {
      segmentId: String(row.segmentDbId),
      playbackStartUnix: targetUnix,
      playbackEndUnix: row.endTimeUnix,
      ...buildReplayBodyBase(),
    }
    const res = await api.startReplay(cameraId.value, body)
    if (res.code !== 0) {
      previewState.value = 'error'
      previewError.value = res.message || '跳转失败'
      return
    }
    replayStreamId.value = res.data?.streamId || res.data?.replayStreamId || null
    const flvUrl = res.data?.flvUrl || res.data?.wsFlvUrl
    if (!flvUrl || !playerContainer.value) {
      previewState.value = 'error'
      previewError.value = '未返回播放地址'
      return
    }
    await nextTick()
    await nextTick()
    playerHandle = await createJessibucaPlayer(playerContainer.value, flvUrl, {
      onError: (err) => {
        previewState.value = 'error'
        previewError.value = `播放错误: ${String(err?.message || err)}`
      },
    })
    applyReplayPlaybackRate()
    replaySessionStartUnix.value = targetUnix
    replaySessionEndUnix.value = row.endTimeUnix
    replayPlayWallStartMs.value = Date.now()
    replayUiTick.value = 0
    startReplayProgressTimer()
    previewState.value = 'playing'
  } catch (e) {
    previewState.value = 'error'
    previewError.value = e?.message || '跳转失败'
    ElMessage.error(previewError.value)
  }
}

const downloadDialogVisible = ref(false)
const downloadRecord = ref(null)
const downloadProgress = ref(0)
const downloadStatus = ref('downloading')
const activeDownloadId = ref(null)
const lastDownloadId = ref(null)
let downloadPollTimer = null

function goBack() {
  if (window.history.length > 1) {
    router.back()
  } else {
    router.push('/live')
  }
}

/**
 * 列表展示：统一为 YYYY-MM-DD HH:mm:ss（去掉 T、毫秒、+08 等后缀）
 */
function formatTime(str) {
  if (str == null || str === '') return ''
  const s = String(str).trim()
  const m = s.match(/^(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2})/)
  if (m) return `${m[1]} ${m[2]}`
  const d = new Date(s)
  if (!Number.isNaN(d.getTime())) {
    const pad = (n) => String(n).padStart(2, '0')
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
  }
  return s.replace('T', ' ').replace(/\+08(:00)?$/, '').replace(/\.\d+Z?$/, '').trim()
}

function formatDurationLabel(sec) {
  const s = Number(sec) || 0
  if (s >= 3600) return `${Math.floor(s / 3600)}小时`
  return `${Math.max(1, Math.floor(s / 60))}分钟`
}

/**
 * 后端列表时间为 Asia/Shanghai 墙钟；按上海解析为 Unix 秒（与 EXTRACT(EPOCH FROM timestamptz) 对齐）。
 */
function unixFromShanghaiWallClock(timeStr) {
  const m = String(timeStr || '').match(/^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})/)
  if (!m) return 0
  const iso = `${m[1]}-${m[2]}-${m[3]}T${m[4]}:${m[5]}:${m[6]}+08:00`
  const ms = new Date(iso).getTime()
  return Number.isFinite(ms) ? Math.floor(ms / 1000) : 0
}

/**
 * 保证列表展示：开始 ≤ 结束；Unix 与字符串用同一语义（上海墙钟）排序，避免与后端 LEAST/GREATEST 错位。
 */
function normalizeReplayRowDisplay(seg) {
  let start = formatTime(seg.startTime)
  let end = formatTime(seg.endTime)
  let startU = Number(seg.startTimeUnix)
  let endU = Number(seg.endTimeUnix)
  const fbS = unixFromShanghaiWallClock(start)
  const fbE = unixFromShanghaiWallClock(end)
  if (!Number.isFinite(startU) || startU <= 0) startU = fbS
  if (!Number.isFinite(endU) || endU <= 0) endU = fbE
  if (startU > endU) {
    const t = start
    start = end
    end = t
    const u = startU
    startU = endU
    endU = u
  }
  let dur = Number(seg.durationSeconds) || 0
  if (!dur && endU > startU) dur = endU - startU
  return {
    startTime: start,
    endTime: end,
    durationSec: dur,
    durationLabel: formatDurationLabel(dur),
    startTimeUnix: startU,
    endTimeUnix: endU,
  }
}

/** 日期选择器字符串 → 本地时间的 ISO8601（带偏移），与后端 timestamptz 一致 */
function parsePickerToDate(s) {
  const [d, t] = s.trim().split(/\s+/)
  const [y, m, day] = d.split('-').map(Number)
  const parts = (t || '0:0:0').split(':')
  const h = Number(parts[0]) || 0
  const min = Number(parts[1]) || 0
  const sec = Number(parts[2]) || 0
  return new Date(y, m - 1, day, h, min, sec)
}

function toIsoWithLocalOffset(d) {
  const pad = (n) => String(n).padStart(2, '0')
  const y = d.getFullYear()
  const m = pad(d.getMonth() + 1)
  const day = pad(d.getDate())
  const h = pad(d.getHours())
  const min = pad(d.getMinutes())
  const s = pad(d.getSeconds())
  const tz = -d.getTimezoneOffset()
  const sign = tz >= 0 ? '+' : '-'
  const abs = Math.abs(tz)
  const hh = pad(Math.floor(abs / 60))
  const mm = pad(abs % 60)
  return `${y}-${m}-${day}T${h}:${min}:${s}${sign}${hh}:${mm}`
}

function buildReplayPlatformQuery() {
  const q = {}
  if (platformGbId.value) q.platformGbId = platformGbId.value
  if (routePlatformId.value) q.platformId = routePlatformId.value
  if (platformDbIdStr.value) q.platformDbId = platformDbIdStr.value
  return q
}

function buildReplayBodyBase() {
  const o = {}
  if (platformGbId.value) o.platformGbId = platformGbId.value
  if (platformDbIdStr.value) o.platformDbId = Number(platformDbIdStr.value)
  if (routePlatformId.value) o.platformId = routePlatformId.value
  return o
}

function onReplaySearchLast1h() {
  const end = new Date()
  const start = new Date(end.getTime() - 3600 * 1000)
  const fmt = (d) => {
    const pad = (n) => String(n).padStart(2, '0')
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
  }
  replayTimeRange.value = [fmt(start), fmt(end)]
  onReplaySearch()
}

function onReplaySearch() {
  const range = replayTimeRange.value
  if (!range || !Array.isArray(range) || range.length < 2) {
    ElMessage.warning('请选择开始、结束时间')
    return
  }
  if (!cameraId.value) {
    ElMessage.warning('缺少摄像头信息')
    return
  }
  const [startStr, endStr] = range
  const startIso = toIsoWithLocalOffset(parsePickerToDate(startStr))
  const endIso = toIsoWithLocalOffset(parsePickerToDate(endStr))
  // 每次检索先清空列表，避免仍显示上一轮结果直至新数据返回
  replayList.value = []
  replayTotal.value = 0
  replayPage.value = 1
  replaySearching.value = true
  replaySearchDone.value = false
  api
    .getReplaySegments(cameraId.value, startIso, endIso, buildReplayPlatformQuery())
    .then((res) => {
      const list = res.data && res.data.items ? res.data.items : []
      replayList.value = list.map((seg) => {
        const d = normalizeReplayRowDisplay(seg)
        return {
          segmentDbId: seg.id,
          recordId: seg.segmentId || String(seg.id),
          startTime: d.startTime,
          endTime: d.endTime,
          durationSec: d.durationSec,
          durationLabel: d.durationLabel,
          startTimeUnix: d.startTimeUnix,
          endTimeUnix: d.endTimeUnix,
          streamType: '主码流',
        }
      })
      replayTotal.value = replayList.value.length
      replayPage.value = 1
      replaySearchDone.value = true
      ElMessage.success(list.length ? `检索完成，共 ${list.length} 条录像` : '该时间段内暂无录像')
    })
    .catch((err) => {
      ElMessage.error(err.message || '检索失败')
      replayList.value = []
      replayTotal.value = 0
      replaySearchDone.value = true
    })
    .finally(() => {
      replaySearching.value = false
    })
}

function onReplaySearchReset() {
  replayTimeRange.value = null
  replayList.value = []
  replaySearchDone.value = false
  replayTotal.value = 0
  replayPage.value = 1
}

async function onClearCameraData() {
  if (!cameraId.value) {
    ElMessage.warning('缺少摄像头信息')
    return
  }
  try {
    await ElMessageBox.confirm(
      '将删除本库中该摄像头的录像检索任务、片段与下载记录、媒体会话缓存、以及 channel_id 匹配的告警。不会删除摄像头档案与目录挂载。是否继续？',
      '清空本机数据',
      { type: 'warning', confirmButtonText: '清空', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  clearingData.value = true
  try {
    const res = await api.clearCameraRelatedData(cameraId.value)
    if (res.code !== 0) {
      throw new Error(res.message || '清空失败')
    }
    ElMessage.success('已清空本机关联数据')
    onReplaySearchReset()
  } catch (e) {
    ElMessage.error(e?.message || '清空失败')
  } finally {
    clearingData.value = false
  }
}

function destroyPlayer() {
  stopReplayProgressTimer()
  if (playerHandle) {
    destroyJessibucaPlayer(playerHandle)
    playerHandle = null
  }
}

async function startReplayPlayback(row) {
  previewState.value = 'loading'
  previewError.value = ''
  destroyPlayer()
  await nextTick()
  await nextTick()
  const body = {
    segmentId: String(row.segmentDbId),
    ...buildReplayBodyBase(),
  }
  const res = await api.startReplay(cameraId.value, body)
  if (res.code !== 0) {
    previewState.value = 'error'
    previewError.value = res.message || '发起回放失败'
    return
  }
  replayStreamId.value = res.data?.streamId || res.data?.replayStreamId || null
  const flvUrl = res.data?.flvUrl || res.data?.wsFlvUrl
  if (!flvUrl) {
    previewState.value = 'error'
    previewError.value = '未返回播放地址'
    return
  }
  if (!playerContainer.value) {
    previewState.value = 'error'
    previewError.value = '播放器容器未准备好'
    return
  }
  await nextTick()
  await nextTick()
  playerHandle = await createJessibucaPlayer(playerContainer.value, flvUrl, {
    onError: (err) => {
      previewState.value = 'error'
      previewError.value = `播放错误: ${String(err?.message || err)}`
    },
  })
  if (row.startTimeUnix && row.endTimeUnix) {
    replaySessionStartUnix.value = row.startTimeUnix
    replaySessionEndUnix.value = row.endTimeUnix
    replayPlayWallStartMs.value = Date.now()
    replayUiTick.value = 0
    startReplayProgressTimer()
  }
  applyReplayPlaybackRate()
  previewState.value = 'playing'
}

async function onPreview(row) {
  previewRecord.value = row
  previewDialogVisible.value = true
  /* 与目录/实时预览一致：弹窗挂载后再起播，避免容器 0×0 黑屏 */
  await nextTick()
  await nextTick()
  try {
    await startReplayPlayback(row)
  } catch (e) {
    previewState.value = 'error'
    previewError.value = e?.message || '连接失败'
    ElMessage.error(previewError.value)
  }
}

async function retryPreview() {
  if (!previewRecord.value) return
  try {
    await startReplayPlayback(previewRecord.value)
    ElMessage.success('已重试')
  } catch (e) {
    previewError.value = e?.message || '重试失败'
  }
}

async function onPreviewDialogClosed() {
  // 关闭对话框时立即发送 BYE
  fireReplayStopNow()
  destroyPlayer()
  replaySessionStartUnix.value = 0
  replaySessionEndUnix.value = 0
  replayPlayWallStartMs.value = 0
  replayUiTick.value = 0
  replayTimelineDragging.value = false
  replayTimelineDragValue.value = 0
  replayPlaybackRate.value = 1
  previewState.value = 'idle'
  previewError.value = ''
  previewRecord.value = null
}

function clearDownloadPoll() {
  if (downloadPollTimer != null) {
    clearTimeout(downloadPollTimer)
    downloadPollTimer = null
  }
}

function fireCancelDownloadNow() {
  // 仅对下载中的任务发 cancel（后端会同时清理 ZLM 文件）
  // done 状态不在此处清理——浏览器可能正在下载，页面关闭后让后端自行超时清理
  const dlId = activeDownloadId.value
  if (!dlId) return

  const url = `/api/replay/download/${encodeURIComponent(dlId)}/cancel`
  if (typeof navigator.sendBeacon === 'function') {
    navigator.sendBeacon(url, new Blob(['{}'], { type: 'application/json' }))
  } else {
    fetch(url, { method: 'POST', keepalive: true }).catch(() => {})
  }
  activeDownloadId.value = null
}

function onDownloadDialogClosed() {
  clearDownloadPoll()
  const dlId = activeDownloadId.value || lastDownloadId.value
  if (!dlId) return

  if (downloadStatus.value === 'downloading') {
    api.cancelReplayDownload(dlId).catch(() => {})
  } else if (downloadStatus.value === 'error') {
    api.cleanupReplayDownload(dlId).catch(() => {})
  } else if (downloadStatus.value === 'done') {
    // 浏览器下载是异步的（a.click() 只是触发，实际传输需要时间），
    // 必须延迟清理以确保浏览器完成文件下载后再删除 ZLM 录制文件。
    const id = dlId
    setTimeout(() => {
      api.cleanupReplayDownload(id).catch(() => {})
    }, 60000)
  }
  activeDownloadId.value = null
  lastDownloadId.value = null
}

async function cancelCurrentDownload() {
  const dlId = activeDownloadId.value
  if (!dlId) return
  clearDownloadPoll()
  try {
    await api.cancelReplayDownload(dlId)
  } catch (_) { /* ignore */ }
  activeDownloadId.value = null
  downloadStatus.value = 'cancelled'
  downloadProgress.value = 0
}

async function pollDownloadOnce(id) {
  if (downloadStatus.value !== 'downloading') return
  const res = await api.getReplayDownloadStatus(id)
  if (res.code !== 0) throw new Error(res.message || '查询失败')
  const { status, progress, downloadUrl } = res.data
  downloadProgress.value = Math.min(100, Number(progress) || 0)
  if (status === 'ready') {
    downloadProgress.value = 100
    downloadStatus.value = 'done'
    lastDownloadId.value = id
    activeDownloadId.value = null
    if (downloadUrl) {
      const a = document.createElement('a')
      a.href = downloadUrl
      a.download = `replay_${id}.mp4`
      a.click()
    } else {
      ElMessage.warning('下载地址为空')
    }
    return
  }
  if (status === 'failed') {
    downloadStatus.value = 'error'
    lastDownloadId.value = id
    activeDownloadId.value = null
    return
  }
  if (status === 'cancelled') {
    downloadStatus.value = 'cancelled'
    activeDownloadId.value = null
    return
  }
  downloadPollTimer = window.setTimeout(() => pollDownloadOnce(id), 800)
}

async function onDownload(row) {
  downloadRecord.value = row
  downloadProgress.value = 0
  downloadStatus.value = 'downloading'
  activeDownloadId.value = null
  downloadDialogVisible.value = true
  clearDownloadPoll()
  try {
    const res = await api.postReplayDownload({
      segmentId: String(row.segmentDbId),
      cameraId: cameraId.value,
      ...buildReplayBodyBase(),
    })
    if (res.code !== 0) throw new Error(res.message || '创建下载任务失败')
    const downloadId = res.data?.downloadId
    if (downloadId == null) throw new Error('未返回 downloadId')
    activeDownloadId.value = downloadId
    lastDownloadId.value = downloadId
    await pollDownloadOnce(downloadId)
  } catch (e) {
    downloadStatus.value = 'error'
    ElMessage.error(e?.message || '下载失败')
  }
}

/**
 * 发送回放 BYE 请求。使用 sendBeacon 作为兜底（页面关闭/刷新时 fetch 可能被取消）。
 * sendBeacon 可靠地在 beforeunload/unload 中排队发送请求。
 */
function fireReplayStopNow() {
  const sid = replayStreamId.value
  const camId = cameraId.value
  if (!sid || !camId) return

  const url = `/api/cameras/${encodeURIComponent(camId)}/replay/stop`
  const payload = JSON.stringify({ streamId: sid })

  // 优先 sendBeacon（浏览器保证在页面卸载后仍发送）
  if (typeof navigator.sendBeacon === 'function') {
    const sent = navigator.sendBeacon(url, new Blob([payload], { type: 'application/json' }))
    if (sent) {
      console.log('[ReplayBye] sendBeacon 已发送 stop, streamId=', sid)
      replayStreamId.value = null
      return
    }
  }

  // sendBeacon 失败时回退到 fetch（keepalive 使其在页面卸载后仍可完成）
  try {
    fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: payload,
      keepalive: true,
    })
    console.log('[ReplayBye] fetch keepalive 已发送 stop, streamId=', sid)
  } catch (e) {
    console.warn('[ReplayBye] stop 发送失败:', e)
  }
  replayStreamId.value = null
}

function onBeforeUnload() {
  fireReplayStopNow()
  fireCancelDownloadNow()
}

onMounted(() => {
  window.addEventListener('beforeunload', onBeforeUnload)
})

onUnmounted(() => {
  window.removeEventListener('beforeunload', onBeforeUnload)
  clearDownloadPoll()
  fireCancelDownloadNow()
  fireReplayStopNow()
  destroyPlayer()
})
</script>

<style scoped>
.page-header {
  margin-bottom: 16px;
}

.replay-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.replay-header-left {
  display: flex;
  align-items: center;
  gap: 12px;
}

.back-btn {
  flex-shrink: 0;
}

.replay-page-title {
  margin: 0;
  line-height: 32px;
}

.replay-search {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.replay-camera-info {
  max-width: 640px;
}

.replay-time-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 12px;
}

.replay-time-picker.el-date-editor {
  --el-date-editor-width: 400px;
  --el-date-editor-datetimerange-width: 400px;
  flex: 0 0 auto !important;
  width: 400px !important;
  max-width: min(400px, 100%) !important;
  min-width: 0 !important;
  box-sizing: border-box;
}

.replay-table {
  background: #fff;
}

.replay-list-tip {
  padding: 12px 0;
  font-size: 12px;
  color: var(--gb-text-soft);
}

.replay-list-footer {
  margin-top: 8px;
  display: flex;
  justify-content: flex-end;
}

.player-container {
  width: 100%;
  height: 420px;
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
  text-align: center;
}

.replay-timeline-bar {
  margin-top: 12px;
  padding: 8px 0;
}

.replay-timeline-row {
  padding: 0 4px;
}

.replay-timeline-meta {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  margin-top: 6px;
  font-size: 12px;
  color: var(--gb-text-soft);
  font-variant-numeric: tabular-nums;
}

.replay-timeline-sep {
  opacity: 0.6;
}

.replay-rate-row {
  margin-top: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 10px;
}

.replay-rate-label {
  font-size: 13px;
  color: var(--gb-text-muted);
}

.replay-rate-select {
  width: 100px;
}

/* 倍速选项样式 */
:deep(.rate-fast) {
  color: #e6a23c;
  font-weight: 500;
}

:deep(.rate-fast)::after {
  content: ' ⚡';
}

:deep(.rate-slow) {
  color: #409eff;
}

.dialog-desc {
  font-size: 13px;
  color: var(--gb-text-soft);
  margin-bottom: 8px;
}

.download-desc {
  font-size: 13px;
  color: var(--gb-text-muted);
  margin-bottom: 12px;
}

.download-status {
  margin-top: 8px;
  font-size: 12px;
  color: var(--gb-text-soft);
}
</style>

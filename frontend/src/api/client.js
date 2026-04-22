// 前端统一 API 封装（首版：支持简单 Mock）

import { authFetch, getToken } from '../router/index.js'

async function realRequest(path, options = {}) {
  const res = await authFetch(path, options)
  const text = await res.text()
  let data = {}
  try {
    data = text ? JSON.parse(text) : {}
  } catch {
    throw new Error(text || `HTTP ${res.status}`)
  }
  if (!res.ok) {
    throw new Error(data.message || res.statusText || `HTTP ${res.status}`)
  }
  return data
}

// --- Mock 数据区域：与 docs/api/http-contracts.md 对齐（占位实现） ---

function mockDelay(data, ms = 300) {
  return new Promise((resolve) => setTimeout(() => resolve(data), ms))
}

export const api = {
  async getLocalGbConfig() {
    return realRequest('/api/config/local-gb')
  },

  async getMediaConfig() {
    return realRequest('/api/config/media')
  },

  async putLocalGbConfig(body) {
    return realRequest('/api/config/local-gb', { method: 'PUT', body: JSON.stringify(body) })
  },

  async putMediaConfig(body) {
    return realRequest('/api/config/media', { method: 'PUT', body: JSON.stringify(body) })
  },

  // 平台管理：下级平台列表，复用 /api/device-platforms
  async listPlatforms(params = {}) {
    const query = new URLSearchParams(params).toString()
    const url = query ? `/api/device-platforms?${query}` : '/api/device-platforms'
    return realRequest(url)
  },

  async listDevicePlatforms(params = {}) {
    const query = new URLSearchParams(params).toString()
    const url = query ? `/api/device-platforms?${query}` : '/api/device-platforms'
    return realRequest(url)
  },

  async postDevicePlatform(body) {
    return realRequest('/api/device-platforms', { method: 'POST', body: JSON.stringify(body) })
  },

  async putDevicePlatform(id, body) {
    return realRequest(`/api/device-platforms/${encodeURIComponent(id)}`, { method: 'PUT', body: JSON.stringify(body) })
  },

  async deleteDevicePlatform(id) {
    return realRequest(`/api/device-platforms/${encodeURIComponent(id)}`, { method: 'DELETE' })
  },

  async queryCatalog(id) {
    return realRequest(`/api/device-platforms/${encodeURIComponent(id)}/catalog-query`, { method: 'POST' })
  },

  // 获取平台目录树（支持设备、目录、行政区域）
  async getCatalogTree(platformId) {
    return realRequest(`/api/catalog/tree?platformId=${encodeURIComponent(platformId)}`)
  },

  async listCameras(params = {}) {
    const query = new URLSearchParams(params).toString()
    const url = query ? `/api/cameras?${query}` : '/api/cameras'
    return realRequest(url)
  },

  async deleteCamera(id) {
    return realRequest(`/api/cameras/${encodeURIComponent(id)}`, { method: 'DELETE' })
  },

  async batchDeleteCameras(cameraIds) {
    return realRequest('/api/cameras/batch-delete', {
      method: 'POST',
      body: JSON.stringify({ cameraIds }),
    })
  },

  /** 清空该摄像头在本库的录像检索缓存、片段与下载记录、媒体会话行、告警（不删 cameras 档案） */
  async clearCameraRelatedData(cameraId) {
    return realRequest(`/api/cameras/${encodeURIComponent(cameraId)}/data/clear`, { method: 'POST' })
  },

  async getCatalogNodes() {
    return realRequest('/api/catalog/nodes')
  },

  async getCatalogNodeCameras(nodeId) {
    return realRequest(`/api/catalog/nodes/${encodeURIComponent(nodeId)}/cameras`)
  },

  async postCatalogNode(body) {
    return realRequest('/api/catalog/nodes', { method: 'POST', body: JSON.stringify(body) })
  },

  async putCatalogNode(nodeId, body) {
    return realRequest(`/api/catalog/nodes/${encodeURIComponent(nodeId)}`, { method: 'PUT', body: JSON.stringify(body) })
  },

  async deleteCatalogNode(nodeId) {
    return realRequest(`/api/catalog/nodes/${encodeURIComponent(nodeId)}`, { method: 'DELETE' })
  },

  async putCatalogNodeCameras(nodeId, body) {
    return realRequest(`/api/catalog/nodes/${encodeURIComponent(nodeId)}/cameras`, { method: 'PUT', body: JSON.stringify(body) })
  },

  /** 本机目录编组树（nested=1 默认嵌套 children） */
  async getCatalogGroupNodes(params = {}) {
    const q = new URLSearchParams({ nested: '1', ...params }).toString()
    return realRequest(`/api/catalog-group/nodes?${q}`)
  },

  async postCatalogGroupNode(body) {
    return realRequest('/api/catalog-group/nodes', { method: 'POST', body: JSON.stringify(body) })
  },

  async putCatalogGroupNode(nodeId, body) {
    return realRequest(`/api/catalog-group/nodes/${encodeURIComponent(nodeId)}`, { method: 'PUT', body: JSON.stringify(body) })
  },

  async deleteCatalogGroupNode(nodeId) {
    return realRequest(`/api/catalog-group/nodes/${encodeURIComponent(nodeId)}`, { method: 'DELETE' })
  },

  async getCatalogGroupNodeCameras(nodeId, params = {}) {
    const q = new URLSearchParams(params).toString()
    const url = q
      ? `/api/catalog-group/nodes/${encodeURIComponent(nodeId)}/cameras?${q}`
      : `/api/catalog-group/nodes/${encodeURIComponent(nodeId)}/cameras`
    return realRequest(url)
  },

  async putCatalogGroupNodeCameras(nodeId, body) {
    return realRequest(`/api/catalog-group/nodes/${encodeURIComponent(nodeId)}/cameras`, {
      method: 'PUT',
      body: JSON.stringify(body),
    })
  },

  async getCatalogGroupCameraMounts(params = {}) {
    const q = new URLSearchParams(params).toString()
    const url = q ? `/api/catalog-group/camera-mounts?${q}` : '/api/catalog-group/camera-mounts'
    return realRequest(url)
  },

  /** 编组节点平铺列表（供上级平台 scope 多选） */
  async getCatalogGroupNodesFlat() {
    return realRequest('/api/catalog-group/nodes?nested=0')
  },

  async getCatalogGroupImportOccupancy(platformId) {
    return realRequest(`/api/catalog-group/import-occupancy?platformId=${encodeURIComponent(platformId)}`)
  },

  async postCatalogGroupImport(body) {
    return realRequest('/api/catalog-group/import', { method: 'POST', body: JSON.stringify(body) })
  },

  /** 上级平台（upstream_platforms），非下级 device-platforms */
  async listUpstreamPlatforms(params = {}) {
    const q = new URLSearchParams(params).toString()
    return realRequest(q ? `/api/platforms?${q}` : '/api/platforms')
  },

  async getUpstreamPlatform(id) {
    return realRequest(`/api/platforms/${encodeURIComponent(id)}`)
  },

  async getPlatformCatalogScope(id) {
    return realRequest(`/api/platforms/${encodeURIComponent(id)}/catalog-scope`)
  },

  async postPlatform(body) {
    return realRequest('/api/platforms', { method: 'POST', body: JSON.stringify(body) })
  },

  async putPlatform(id, body) {
    return realRequest(`/api/platforms/${encodeURIComponent(id)}`, { method: 'PUT', body: JSON.stringify(body) })
  },

  /**
   * @param {number[]|{ catalogGroupNodeIds: number[], excludedCameraIds?: number[] }} catalogGroupNodeIdsOrBody
   */
  async putPlatformCatalogScope(id, catalogGroupNodeIdsOrBody) {
    const body =
      catalogGroupNodeIdsOrBody && typeof catalogGroupNodeIdsOrBody === 'object' && !Array.isArray(catalogGroupNodeIdsOrBody)
        ? catalogGroupNodeIdsOrBody
        : { catalogGroupNodeIds: catalogGroupNodeIdsOrBody || [] }
    return realRequest(`/api/platforms/${encodeURIComponent(id)}/catalog-scope`, {
      method: 'PUT',
      body: JSON.stringify(body),
    })
  },

  async postUpstreamCatalogNotify(id) {
    return realRequest(`/api/platforms/${encodeURIComponent(id)}/catalog-notify`, { method: 'POST' })
  },

  async deletePlatform(id) {
    return realRequest(`/api/platforms/${encodeURIComponent(id)}`, { method: 'DELETE' })
  },

  async getOverview() {
    return realRequest('/api/overview')
  },

  async listAlarms(params = {}) {
    const query = new URLSearchParams(params).toString()
    const url = query ? `/api/alarms?${query}` : '/api/alarms'
    return realRequest(url)
  },

  async postAlarm(body) {
    return realRequest('/api/alarms', { method: 'POST', body: JSON.stringify(body) })
  },

  async putAlarm(id, body) {
    return realRequest(`/api/alarms/${encodeURIComponent(id)}`, { method: 'PUT', body: JSON.stringify(body) })
  },

  /**
   * @param {string} cameraId
   * @param {string} startTime ISO8601 带时区
   * @param {string} endTime ISO8601 带时区
   * @param {{ platformGbId?: string, platformId?: string, platformDbId?: string }} [extra]
   */
  async getReplaySegments(cameraId, startTime, endTime, extra = {}) {
    const params = new URLSearchParams({ cameraId, startTime, endTime })
    if (extra.platformGbId) params.set('platformGbId', extra.platformGbId)
    if (extra.platformId) params.set('platformId', String(extra.platformId))
    if (extra.platformDbId) params.set('platformDbId', String(extra.platformDbId))
    return realRequest(`/api/replay/segments?${params}`)
  },

  /**
   * 录像回放起停（国标 Playback + ZLM），与预览共用 Jessibuca
   * @param {string} cameraId
   * @param {{ segmentId: string, platformGbId?: string, platformDbId?: number, platformId?: string, playbackStartUnix?: number, playbackEndUnix?: number, offsetSeconds?: number }} body
   * `playbackStartUnix`/`playbackEndUnix` 与 `offsetSeconds` 互斥；均为可选，缺省按整段起播。见 `docs/api/http-contracts.md`。
   */
  async startReplay(cameraId, body) {
    return realRequest(`/api/cameras/${encodeURIComponent(cameraId)}/replay/start`, {
      method: 'POST',
      body: JSON.stringify(body),
    })
  },

  async stopReplay(cameraId, body) {
    return realRequest(`/api/cameras/${encodeURIComponent(cameraId)}/replay/stop`, {
      method: 'POST',
      body: JSON.stringify(body),
    })
  },

  /**
   * 回放倍速控制（SIP INFO + MANSRTSP），通知设备端改变发送速率
   * @param {string} cameraId
   * @param {{ streamId: string, scale: number }} body  scale: 0.25/0.5/1/2/4/8/16/32
   */
  async setReplaySpeed(cameraId, body) {
    return realRequest(`/api/cameras/${encodeURIComponent(cameraId)}/replay/speed`, {
      method: 'POST',
      body: JSON.stringify(body),
    })
  },

  async getReplaySession(cameraId, streamId) {
    const q = new URLSearchParams({ streamId })
    return realRequest(`/api/cameras/${encodeURIComponent(cameraId)}/replay/session?${q}`)
  },

  async postReplayDownload(body) {
    return realRequest('/api/replay/download', { method: 'POST', body: JSON.stringify(body) })
  },

  async getReplayDownloadStatus(id) {
    return realRequest(`/api/replay/download/${encodeURIComponent(id)}`)
  },

  /**
   * 取消进行中的下载任务（后端终止 SIP 会话 + ZLM 录制 + 释放资源）
   * @param {number|string} id downloadId
   */
  async cancelReplayDownload(id) {
    return realRequest(`/api/replay/download/${encodeURIComponent(id)}/cancel`, { method: 'POST' })
  },

  /**
   * 浏览器下载完成后清理 ZLM 录制文件
   * @param {number|string} id downloadId
   */
  async cleanupReplayDownload(id) {
    return realRequest(`/api/replay/download/${encodeURIComponent(id)}/cleanup`, { method: 'POST' })
  },

  /** 浏览器下载录像文件（需自行带 Authorization，见 fetchReplayDownloadBlob） */
  replayDownloadFilePath(id) {
    return `/api/replay/download/${encodeURIComponent(id)}/file`
  },

  async fetchReplayDownloadBlob(id) {
    const token = getToken()
    const headers = {}
    if (token) headers.Authorization = `Bearer ${token}`
    const res = await fetch(`/api/replay/download/${encodeURIComponent(id)}/file`, { headers })
    if (!res.ok) {
      const t = await res.text().catch(() => '')
      throw new Error(t || `HTTP ${res.status}`)
    }
    return res.blob()
  },

  /**
   * 实时预览起流。路径 `cameraId` 为 **cameras 表库内主键**（与列表项 `id` 一致，通常为小整数）；
   * 亦可传国标通道 ID（20 位等），此时须带 `platformGbId`/`platformDbId` 消歧。
   */
  async startPreview(cameraId, platformGbId, platformDbId, deviceGbId) {
    const body = { cameraId, platformGbId, platformDbId }
    if (deviceGbId != null && String(deviceGbId).trim() !== '') body.deviceGbId = String(deviceGbId).trim()
    return realRequest(`/api/cameras/${encodeURIComponent(cameraId)}/preview/start`, {
      method: 'POST',
      body: JSON.stringify(body),
    })
  },

  async stopPreview(cameraId, platformGbId, platformDbId, deviceGbId) {
    const body = { cameraId, platformGbId, platformDbId }
    if (deviceGbId != null && String(deviceGbId).trim() !== '') body.deviceGbId = String(deviceGbId).trim()
    return realRequest(`/api/cameras/${encodeURIComponent(cameraId)}/preview/stop`, {
      method: 'POST',
      body: JSON.stringify(body),
    })
  },

  /** 云台控制（国标 DeviceControl，经后端 SIP MESSAGE 下发） */
  async postPtz(body) {
    return realRequest('/api/ptz', { method: 'POST', body: JSON.stringify(body) })
  },
}


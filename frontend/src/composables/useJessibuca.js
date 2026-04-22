/**
 * @file useJessibuca.js
 * @brief 开源 Jessibuca（langhuihui/jessibuca v3）脚本加载与多实例播放器封装
 * @details 供实时预览、目录、电视墙、录像回放等复用；支持 stats 回调与 video 元素轮询分辨率
 */

export const JESSIBUCA_SCRIPT_URL = '/jessibuca/jessibuca.js'
export const JESSIBUCA_DECODER_URL = '/jessibuca/decoder.js'

let jessibucaLoadingPromise = null

function getJessibucaConstructor() {
  if (typeof window === 'undefined') return null
  return window.Jessibuca || window['Jessibuca'] || null
}

/**
 * @brief 动态加载 UMD 脚本并返回播放器构造函数
 */
export async function loadJessibuca() {
  const existing = getJessibucaConstructor()
  if (existing) return existing

  if (!jessibucaLoadingPromise) {
    jessibucaLoadingPromise = new Promise((resolve, reject) => {
      const existingScript = document.querySelector(`script[src="${JESSIBUCA_SCRIPT_URL}"]`)
      if (existingScript) {
        existingScript.addEventListener(
          'load',
          () => resolve(getJessibucaConstructor()),
          { once: true },
        )
        existingScript.addEventListener(
          'error',
          () => reject(new Error('Jessibuca 脚本加载失败')),
          { once: true },
        )
        return
      }
      const script = document.createElement('script')
      script.src = JESSIBUCA_SCRIPT_URL
      script.async = true
      script.onload = () => resolve(getJessibucaConstructor())
      script.onerror = () => reject(new Error('Jessibuca 脚本加载失败'))
      document.head.appendChild(script)
    }).finally(() => {
      if (!getJessibucaConstructor()) {
        jessibucaLoadingPromise = null
      }
    })
  }

  const C = await jessibucaLoadingPromise
  if (C) return C
  throw new Error('Jessibuca 加载失败')
}

/**
 * @brief 从 stats 与 video 元素归一化 OSD 字段
 */
export function mergeVideoStats(statsObj, containerEl) {
  let w = statsObj?.width ?? statsObj?.videoWidth
  let h = statsObj?.height ?? statsObj?.videoHeight
  const vid = containerEl?.querySelector?.('video')
  if (vid && vid.videoWidth > 0 && vid.videoHeight > 0) {
    w = vid.videoWidth
    h = vid.videoHeight
  }
  const fps = statsObj?.fps ?? statsObj?.FPS ?? statsObj?.frameRate
  const kbps =
    statsObj?.kbps ??
    statsObj?.speed ??
    statsObj?.bitrate ??
    statsObj?.bandwidth ??
    statsObj?.kBps
  const codec =
    statsObj?.videoCodec ??
    statsObj?.codec ??
    statsObj?.videoMime ??
    statsObj?.mime

  return {
    resolution: w && h ? `${w}x${h}` : '—',
    fps: fps != null && fps !== '' ? String(fps) : '—',
    kbps:
      kbps != null && kbps !== ''
        ? typeof kbps === 'number'
          ? `${kbps.toFixed(0)} kbps`
          : String(kbps)
        : '—',
    codec: codec ? String(codec) : '—',
  }
}

const DEFAULT_PLAYER_OPTIONS = {
  videoBuffer: 0.2,
  decoder: JESSIBUCA_DECODER_URL,
  /** 与官方 demo 一致：关闭离屏时部分环境下 canvas 与容器对齐更稳定 */
  forceNoOffscreen: true,
  isResize: false,
  isFlv: true,
  useMSE: false,
  useWCS: false,
  showBandwidth: false,
  autoWasm: true,
  operateBtns: {
    fullscreen: false,
    screenshot: false,
    play: false,
    audio: false,
    record: false,
  },
}

/**
 * el-dialog 等场景首帧容器常为 0×0，若立即 new Jessibuca 会导致画面缩在角上或全黑
 */
function waitForContainerLayout(container, timeoutMs = 2500) {
  return new Promise((resolve) => {
    const t0 = Date.now()
    const done = () => resolve()
    const tick = () => {
      const r = container?.getBoundingClientRect?.()
      if (r && r.width >= 32 && r.height >= 32) {
        done()
        return
      }
      if (Date.now() - t0 >= timeoutMs) {
        done()
        return
      }
      requestAnimationFrame(tick)
    }
    tick()
  })
}

/**
 * @param {HTMLElement} container
 * @param {string} flvUrl
 * @param {object} hooks
 * @param {function} hooks.onStats — 收到统计或轮询时调用 mergeVideoStats 后的对象
 * @param {function} hooks.onError
 */
export async function createJessibucaPlayer(container, flvUrl, hooks = {}) {
  const { onStats, onError } = hooks
  const PlayerClass = await loadJessibuca()
  if (!PlayerClass) throw new Error('Jessibuca 不可用')
  if (!container) throw new Error('播放器容器未准备好')

  await waitForContainerLayout(container)

  let lastStats = {}
  const pushStats = () => {
    if (onStats) onStats(mergeVideoStats(lastStats, container))
  }

  const player = new PlayerClass({
    container,
    ...DEFAULT_PLAYER_OPTIONS,
  })

  const doResize = () => {
    try {
      if (typeof player.resize === 'function') player.resize()
    } catch (_) {
      /* ignore */
    }
  }

  /** 弹窗尺寸变化、全屏等 */
  let resizeObserver = null
  if (typeof ResizeObserver !== 'undefined') {
    resizeObserver = new ResizeObserver(() => {
      doResize()
    })
    try {
      resizeObserver.observe(container)
    } catch (_) {
      resizeObserver = null
    }
  }

  const mergeStatPayload = (payload) => {
    if (payload != null && typeof payload === 'object') {
      lastStats = { ...lastStats, ...payload }
    } else if (typeof payload === 'number') {
      lastStats = { ...lastStats, kbps: payload }
    } else {
      lastStats = { ...lastStats, raw: payload }
    }
    pushStats()
  }

  if (typeof player.on === 'function') {
    try {
      player.on('error', (err) => {
        onError?.(err)
      })
    } catch (_) {
      /* ignore */
    }
    try {
      player.on('stats', mergeStatPayload)
    } catch (_) {
      /* ignore */
    }
    try {
      player.on('videoInfo', (info) => {
        mergeStatPayload(info)
        doResize()
      })
    } catch (_) {
      /* ignore */
    }
    try {
      player.on('kBps', (v) => mergeStatPayload(v))
    } catch (_) {
      /* ignore */
    }
    try {
      player.on('performance', (p) => mergeStatPayload({ performance: p }))
    } catch (_) {
      /* ignore */
    }
    try {
      player.on('start', () => {
        pushStats()
        doResize()
      })
    } catch (_) {
      /* ignore */
    }
  }

  const pollTimer = window.setInterval(() => {
    pushStats()
  }, 800)

  try {
    await player.play(flvUrl)
    const delays = [0, 16, 50, 120, 280, 600]
    delays.forEach((ms) => {
      window.setTimeout(() => doResize(), ms)
    })
    requestAnimationFrame(() => {
      doResize()
      requestAnimationFrame(doResize)
    })
  } catch (err) {
    window.clearInterval(pollTimer)
    if (resizeObserver) {
      try {
        resizeObserver.disconnect()
      } catch (_) {
        /* ignore */
      }
      resizeObserver = null
    }
    onError?.(err)
    try {
      player.destroy?.()
    } catch (_) {
      /* ignore */
    }
    throw err
  }

  /**
   * 录像回放倍速（直播页可不调用）。优先实例 API，否则回退到 video 元素。
   * @param {number} rate 如 0.5 / 1 / 1.25 / 1.5 / 2 / 4 / 8 / 16 / 32
   * @returns {boolean} 是否设置成功
   */
  function setPlaybackRate(rate) {
    const r = Number(rate)
    if (!Number.isFinite(r) || r <= 0) {
      console.warn('[Jessibuca] 无效倍速:', rate)
      return false
    }

    try {
      if (typeof player.setPlaybackRate === 'function') {
        player.setPlaybackRate(r)
        return true
      }
    } catch (e) {
      console.warn('[Jessibuca] player.setPlaybackRate 失败:', e)
    }

    try {
      if ('playbackSpeed' in player) {
        player.playbackSpeed = r
        return true
      }
    } catch (e) {
      console.warn('[Jessibuca] playbackSpeed 设置失败:', e)
    }

    const vid = container?.querySelector?.('video')
    if (vid) {
      try {
        if (vid.readyState >= 1) {
          vid.playbackRate = r
          return true
        }
        const onCanPlay = () => {
          try {
            vid.playbackRate = r
          } catch (err) {
            console.warn('[Jessibuca] 延迟设置倍速失败:', err)
          }
          vid.removeEventListener('canplay', onCanPlay)
        }
        vid.addEventListener('canplay', onCanPlay)
      } catch (e) {
        console.warn('[Jessibuca] video.playbackRate 设置失败:', e)
      }
    }

    console.warn('[Jessibuca] 倍速设置失败，播放器可能未就绪')
    return false
  }

  function destroy() {
    window.clearInterval(pollTimer)
    if (resizeObserver) {
      try {
        resizeObserver.disconnect()
      } catch (_) {
        /* ignore */
      }
      resizeObserver = null
    }
    try {
      const d = player.destroy?.()
      if (d != null && typeof d.then === 'function') {
        d.catch(() => {})
      }
    } catch (e) {
      console.error('Jessibuca destroy error', e)
    }
  }

  return { player, destroy, setPlaybackRate }
}

export function destroyJessibucaPlayer(handle) {
  if (handle && typeof handle.destroy === 'function') {
    handle.destroy()
  }
}

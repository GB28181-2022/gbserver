/**
 * @file dateDisplay.js
 * @brief 将接口返回的时间统一格式化为本地可读字符串
 * @details 后端 device-platforms 等返回 UTC 的 YYYY-MM-DD HH:mm:ss；历史数据可能为 ISO（含 T/Z）
 */

/**
 * @param {string|number|null|undefined} apiValue
 * @returns {string} 本地时区 YYYY-MM-DD HH:mm:ss，空则 —
 */
export function formatLocalDateTime(apiValue) {
  if (apiValue == null || String(apiValue).trim() === '') return '—'
  const s = String(apiValue).trim()

  const pad2 = (n) => String(n).padStart(2, '0')
  const fmtLocal = (d) =>
    `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`

  // 后端：UTC 墙钟，空格分隔（无 T/Z）
  const utcSpace = s.match(/^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})$/)
  if (utcSpace) {
    const d = new Date(
      Date.UTC(
        +utcSpace[1],
        +utcSpace[2] - 1,
        +utcSpace[3],
        +utcSpace[4],
        +utcSpace[5],
        +utcSpace[6],
      ),
    )
    if (!Number.isNaN(d.getTime())) return fmtLocal(d)
  }

  // ISO：2026-03-24T06:30:58Z / 带毫秒 / 带偏移
  let iso = s.includes('T') ? s : s.replace(' ', 'T')
  if (!/[zZ]$/.test(iso) && !/[+-]\d{2}:?\d{2}$/.test(iso)) iso += 'Z'
  const d = new Date(iso)
  if (!Number.isNaN(d.getTime())) return fmtLocal(d)

  return s
}

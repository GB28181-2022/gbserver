/**
 * @file catalogTreeGb28181.js
 * @brief GB28181 附录 O/J 目录树：复合 ParentID、BusinessGroupID、CivilCode 挂边与子树摄像机统计
 */

/**
 * @param {string|null|undefined} raw
 * @param {Set<string>} idSet
 * @returns {string|null} 能在 idSet 中命中的父节点 nodeId，否则 null
 */
export function resolveEffectiveParentId(raw, idSet, platformGbId) {
  if (raw == null || raw === '') return null
  const t = String(raw).trim()
  if (!t) return null
  // 先命中本次目录集中的真实父节点；解决 nodeId == platformGbId 时被误判为“无父”的问题
  if (idSet.has(t)) return t
  const plat = platformGbId != null ? String(platformGbId).trim() : ''
  // 仅指向平台国标、且平台不在本次目录节点集中：视为无有效 ParentID，走 BusinessGroupID
  if (plat && t === plat) return null
  if (t.includes('/')) {
    const parts = t.split('/').filter(Boolean)
    const last = parts[parts.length - 1]
    if (last && idSet.has(last)) return last
  }
  return null
}

/** 从接口项读取 parentId（camel / snake） */
function pickParentId(it) {
  const v = it.parentId ?? it.parent_id
  if (v == null || v === '') return null
  const s = String(v).trim()
  return s || null
}

/** 从接口项读取 businessGroupId（camel / snake） */
function pickBusinessGroupId(it) {
  const v = it.businessGroupId ?? it.business_group_id
  if (v == null || v === '') return null
  const s = String(v).trim()
  return s || null
}

/**
 * @param {object} deviceNode
 * @param {object[]} nodes
 * @returns {string|null} 父节点 nodeId
 */
function findCivilCodeParent(deviceNode, nodes) {
  const cc = (deviceNode.civilCode || '').trim()
  if (!cc) return null
  for (const n of nodes) {
    if (n.isDevice) continue
    if (n.nodeId === cc) return n.nodeId
  }
  for (const n of nodes) {
    if (n.isDevice) continue
    const nc = (n.civilCode || '').trim()
    if (nc && nc === cc) return n.nodeId
  }
  return null
}

/**
 * @param {Array<object>} items - /api/catalog/tree 的 data.items
 * @param {{ platformDbId: string|number, platformGbId: string }} ctx
 * @returns {object[]} el-tree 根节点数组
 */
export function buildCatalogForestGb28181(items, ctx) {
  const list = items || []
  const idSet = new Set(list.map((it) => it.nodeId).filter(Boolean))

  const platformGbId = ctx.platformGbId != null ? String(ctx.platformGbId).trim() : ''

  const nodes = list.map((it) => {
    const isCameraByFlag = it.isCamera === true || it.isCamera === 1 || it.isCamera === 'true'
    const isDevice =
      isCameraByFlag ||
      ((it.isCamera === undefined || it.isCamera === null) &&
        (Number(it.type) === 0 || String(it.typeName || '').toLowerCase() === 'device'))
    const camOn = it.cameraOnline === true || it.cameraOnline === 1 || it.cameraOnline === 'true'
    const bgId = pickBusinessGroupId(it)
    return {
      key: `${ctx.platformDbId}:${it.nodeId}`,
      baseLabel: it.name || it.nodeId,
      label: it.name || it.nodeId,
      nodeId: it.nodeId,
      parentId: pickParentId(it),
      businessGroupId: bgId,
      civilCode: it.civilCode || '',
      type: it.type,
      typeName: it.typeName,
      isDevice,
      cameraOnline: camOn,
      isPlatform: false,
      platformDbId: ctx.platformDbId,
      platformGbId: ctx.platformGbId,
      children: [],
      leaf: isDevice,
      camTotal: 0,
      camOnlineCount: 0,
    }
  })

  const map = new Map(nodes.map((n) => [n.nodeId, n]))
  /** @type {Map<string, string>} childId -> parentId */
  const parentOf = new Map()

  for (const n of nodes) {
    // 1) parent_id 优先：仅当能解析到本次列表中的某节点 nodeId 时才挂父
    let pkey = resolveEffectiveParentId(n.parentId, idSet, platformGbId)
    // 2) 无有效 ParentID 时，按业务分组挂到 business_group_id 对应节点（附录 O.2 / J.2）
    if (!pkey && n.businessGroupId) {
      const bg = String(n.businessGroupId).trim()
      if (bg && idSet.has(bg) && bg !== n.nodeId) {
        pkey = bg
      }
    }
    // 3) 设备仍孤儿：CivilCode 与行政区划节点关联（附录 J.1）
    if (!pkey && n.isDevice) {
      pkey = findCivilCodeParent(n, nodes)
    }
    if (pkey && map.has(pkey) && pkey !== n.nodeId) {
      parentOf.set(n.nodeId, pkey)
    }
  }

  const roots = []
  for (const n of nodes) {
    const p = parentOf.get(n.nodeId)
    if (!p) roots.push(n)
    else map.get(p).children.push(n)
  }

  function fixLeaf(ns) {
    for (const x of ns) {
      if (x.children?.length) {
        x.leaf = false
        fixLeaf(x.children)
      } else if (!x.isDevice) {
        x.leaf = true
      }
    }
  }
  fixLeaf(roots)

  function sortChildren(ns) {
    ns.sort((a, b) => {
      if (a.isDevice !== b.isDevice) return a.isDevice ? 1 : -1
      return (a.baseLabel || '').localeCompare(b.baseLabel || '', 'zh-CN')
    })
    for (const x of ns) {
      if (x.children?.length) sortChildren(x.children)
    }
  }
  sortChildren(roots)

  function aggregateNode(x) {
    if (x.isDevice) {
      x.camTotal = 1
      x.camOnlineCount = x.cameraOnline ? 1 : 0
      x.label = x.baseLabel
      return
    }
    let t = 0
    let o = 0
    for (const c of x.children) {
      aggregateNode(c)
      t += c.camTotal
      o += c.camOnlineCount
    }
    x.camTotal = t
    x.camOnlineCount = o
    x.label = `${x.baseLabel} (${x.camOnlineCount}/${x.camTotal})`
  }

  for (const r of roots) aggregateNode(r)

  return roots
}

-- 编组节点类型扩展：区分 GB28181 业务分组(215) 与 虚拟组织(216)
-- node_type: 0=通道占位 1=虚拟组织/目录(216) 2=行政区域(218) 3=业务分组(215)
-- 历史数据：此前 node_type=1 分配的类型位均为 215，迁移为 3（业务分组）

ALTER TABLE catalog_group_nodes DROP CONSTRAINT IF EXISTS catalog_group_nodes_node_type_check;

ALTER TABLE catalog_group_nodes
  ADD CONSTRAINT catalog_group_nodes_node_type_check CHECK (node_type >= 0 AND node_type <= 3);

-- 仅把「类型位为 215」的旧目录行记为业务分组；已为 216 发号且 node_type=1 的行保持不变
UPDATE catalog_group_nodes
SET node_type = 3
WHERE node_type = 1
  AND gb_device_id IS NOT NULL
  AND length(trim(gb_device_id)) >= 13
  AND substring(trim(gb_device_id) FROM 11 FOR 3) = '215';

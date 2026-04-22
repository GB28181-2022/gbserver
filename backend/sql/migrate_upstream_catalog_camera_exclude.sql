-- 上级目录推送：在 scope 子树内排除指定摄像头
CREATE TABLE IF NOT EXISTS upstream_catalog_camera_exclude (
  upstream_platform_id BIGINT NOT NULL REFERENCES upstream_platforms(id) ON DELETE CASCADE,
  camera_id            BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  created_at           TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (upstream_platform_id, camera_id)
);

CREATE INDEX IF NOT EXISTS idx_upstream_cat_cam_excl_upstream
  ON upstream_catalog_camera_exclude (upstream_platform_id);

<template>
  <div class="page">
    <header class="page-header">
      <div>
        <h2 class="page-title">系统配置</h2>
        <p class="page-subtitle">
          统一管理本机国标编码信息与流媒体接入参数。
        </p>
      </div>
    </header>

    <section class="page-section">
      <el-card shadow="hover">
        <template #header>
          <span>本地国标信息</span>
        </template>
        <el-form
          :model="form"
          label-width="120px"
          class="form"
        >
          <el-form-item label="平台/设备 ID">
            <el-input v-model="form.gbId" placeholder="34020000002000000001" />
          </el-form-item>
          <el-form-item label="SIP 域">
            <el-input v-model="form.sipDomain" placeholder="3402000000" />
          </el-form-item>
          <el-form-item label="信令 IP">
            <el-input v-model="form.sipIp" placeholder="192.168.1.9" />
          </el-form-item>
          <el-form-item label="信令端口">
            <el-input
              v-model.number="form.sipPort"
              type="number"
              placeholder="5060"
            />
          </el-form-item>
          <el-form-item label="用户名">
            <el-input v-model="form.gbUsername" placeholder="admin" />
          </el-form-item>
          <el-form-item label="鉴权密码">
            <el-input v-model="form.gbPassword" />
          </el-form-item>
          <el-form-item label="是否开启 TCP" prop="sipEnableTcp">
            <el-switch v-model="form.sipEnableTcp" />
            <span class="form-item-hint">开启后同时使用 UDP 与 TCP</span>
          </el-form-item>
        </el-form>
      </el-card>
    </section>

    <section class="page-section">
      <el-card shadow="hover">
        <template #header>
          <span>流媒体配置</span>
        </template>
        <el-form
          :model="form"
          label-width="120px"
          class="form form-streaming"
        >
          <el-form-item label="流媒体 IP" prop="playbackHost">
            <el-input v-model="form.playbackHost" placeholder="192.168.1.9" />
            <p class="field-footnote">浏览器拉流使用的对外主机（HTTP-FLV / WS-FLV 的 host）。</p>
          </el-form-item>
          <el-form-item label="起始端口" prop="mediaPortStart">
            <el-input
              v-model.number="form.mediaPortStart"
              type="number"
              placeholder="10000"
            />
          </el-form-item>
          <el-form-item label="结束端口" prop="mediaPortEnd">
            <el-input
              v-model.number="form.mediaPortEnd"
              type="number"
              placeholder="20000"
            />
          </el-form-item>
          <el-form-item label="媒体传输协议" prop="mediaProto">
            <el-select v-model="form.mediaProto" placeholder="请选择" style="width: 100%">
              <el-option label="UDP" value="udp" />
              <el-option label="TCP" value="tcp" />
            </el-select>
            <p class="field-footnote">用于对下级点播 INVITE 的 SDP 及 ZLM 收流模式；未在平台独立配置中覆盖时采用此处默认值。</p>
          </el-form-item>
          <!-- for="" 避免 <label for> 与页面上其它控件 id 错误关联（如 el-switch 内 checkbox） -->
          <el-form-item label="流媒体 API 地址" prop="mediaApiUrl" for="">
            <el-input
              v-model="form.mediaApiUrl"
              placeholder="http://127.0.0.1:880"
            />
            <p class="field-footnote">国标服务调用 ZLMediaKit HTTP API 的根地址（含协议与端口）。</p>
          </el-form-item>
        </el-form>

        <!-- 不用 el-form-item，避免与上方表单项联动时出现多余控件占位 -->
        <div class="streaming-extras form-streaming">
          <div class="streaming-extra-row">
            <div class="streaming-extra-label">ZLM API 密钥</div>
            <div class="streaming-extra-content">
              <div class="zlm-secret-block">
                <p class="zlm-secret-status">
                  {{
                    form.zlmApiSecretConfigured
                      ? '已在服务器配置'
                      : '未配置（需运维在数据库写入 zlm_secret，与 ZLM config.ini 一致）'
                  }}
                </p>
                <p class="zlm-secret-hint">
                  页面不可修改密钥。保存时会调用 ZLM <code>setServerConfig</code> 同步 RTP 端口区间（<code>rtp_proxy.port_range</code>）。
                </p>
                <p class="zlm-secret-hint zlm-secret-hint--muted">
                  保存后写入数据库并同步 ZLMediaKit；若修改 API 根地址或 ZLM 监听端口，ZLM 侧如需重启请自行处理。
                </p>
              </div>
            </div>
          </div>
          <div class="streaming-actions">
            <el-button @click="onReset">重置</el-button>
            <el-button type="primary" @click="onSave" :loading="saving">
              保存
            </el-button>
          </div>
        </div>
      </el-card>
    </section>
  </div>
</template>

<script setup>
import { reactive, ref, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import { api } from '../api/client'

const initial = {
  gbId: '34020000002000000001',
  sipDomain: '3402000000',
  sipIp: '192.168.1.9',
  sipPort: 5060,
  gbUsername: '',
  gbPassword: '',
  sipEnableTcp: false,
  playbackHost: '192.168.1.9',
  mediaPortStart: 10000,
  mediaPortEnd: 20000,
  mediaProto: 'udp',
  mediaApiUrl: 'http://127.0.0.1:880',
  zlmApiSecretConfigured: false,
}

const form = reactive({ ...initial })
const saving = ref(false)

async function loadConfig() {
  try {
    const [gbRes, mediaRes] = await Promise.all([
      api.getLocalGbConfig(),
      api.getMediaConfig(),
    ])
    const gb = gbRes?.data || {}
    const media = mediaRes?.data || {}
    form.gbId = gb.gbId ?? initial.gbId
    form.sipDomain = gb.domain ?? initial.sipDomain
    form.sipIp = gb.signalIp ?? initial.sipIp
    form.sipPort = gb.signalPort ?? initial.sipPort
    form.gbUsername = gb.username ?? ''
    form.gbPassword = gb.password ?? ''
    form.sipEnableTcp = gb.transport?.tcp ?? false
    form.playbackHost = media.playbackHost ?? initial.playbackHost
    form.mediaApiUrl = media.mediaApiUrl ?? initial.mediaApiUrl
    const rtp = media.rtpPortRange || {}
    form.mediaPortStart = rtp.start ?? initial.mediaPortStart
    form.mediaPortEnd = rtp.end ?? initial.mediaPortEnd
    form.zlmApiSecretConfigured = !!media.zlmApiSecretConfigured
    form.mediaProto = media.rtpTransport === 'tcp' ? 'tcp' : 'udp'
  } catch (e) {
    ElMessage.error('加载配置失败：' + (e.message || '网络错误'))
  }
}

function onReset() {
  Object.assign(form, initial)
}

async function onSave() {
  if (form.mediaPortStart > form.mediaPortEnd) {
    ElMessage.warning('起始端口不能大于结束端口')
    return
  }
  saving.value = true
  try {
    // 先保存本地国标配置
    await api.putLocalGbConfig({
      gbId: form.gbId,
      domain: form.sipDomain,
      name: form.gbId || '',
      username: form.gbUsername,
      password: form.gbPassword,
      signalIp: form.sipIp,
      signalPort: form.sipPort,
      // 与 docs/api/http-contracts.md 一致；嵌套 transport 供后端优先解析，避免与 password 等字段中的子串误匹配
      udp: true,
      tcp: form.sipEnableTcp,
      transport: {
        udp: true,
        tcp: form.sipEnableTcp,
      },
    })
    // 注意：保存本地国标后不能在此处调用 loadConfig()，
    // 否则会从服务器重新拉取数据，覆盖用户在页面上修改的流媒体配置字段
    try {
      // 使用用户在页面输入的值保存流媒体配置
      const mediaRes = await api.putMediaConfig({
        start: form.mediaPortStart,
        end: form.mediaPortEnd,
        playbackHost: form.playbackHost,
        mediaApiUrl: form.mediaApiUrl,
        rtpTransport: form.mediaProto === 'tcp' ? 'tcp' : 'udp',
      })
      // 两项配置都已保存，最后统一加载一次以显示服务器端的最新值
      await loadConfig()
      // 显示ZLM连接检测结果
      const zlmCheck = mediaRes?.data?.zlmCheck
      if (zlmCheck && !zlmCheck.connected) {
        ElMessage.warning(
          '配置已保存，但流媒体连接检测失败：' + (zlmCheck.message || '未知错误') +
          '。请检查 ZLMediaKit 是否运行、API地址是否正确。'
        )
      } else {
        ElMessage.success(
          '配置已保存。若修改了「是否开启 TCP」，请重启 gb_service 后信令才会在对应端口上监听或关闭 TCP。',
        )
      }
    } catch (me) {
      // 流媒体保存失败时，也要刷新一次以确保页面状态正确
      await loadConfig()
      ElMessage.warning(
        '本地国标已保存，但流媒体保存失败：' +
          (me.message || '未知错误') +
          '。若仅改了 TCP 开关，重启 gb_service 即可使信令传输生效。',
      )
    }
  } catch (e) {
    ElMessage.error('保存失败：' + (e.message || '网络错误'))
  } finally {
    saving.value = false
  }
}

onMounted(loadConfig)
</script>

<style scoped>
.page-header {
  margin-bottom: 18px;
}

.form {
  max-width: 600px;
}

/* 与浏览器调试一致：标签区 width:120px 按内容盒计算，避免 border-box 挤占对齐 */
.form-streaming :deep(.el-form-item__label) {
  box-sizing: content-box;
}

.form-streaming .zlm-secret-block {
  line-height: 1.5;
}

.form-streaming .zlm-secret-status {
  margin: 0 0 8px;
  font-size: 14px;
  font-weight: 600;
  color: var(--gb-text-main, #303133);
}

.form-streaming .zlm-secret-hint {
  margin: 0 0 6px;
  font-size: 12px;
  color: var(--gb-text-soft, #909399);
}

.form-streaming .zlm-secret-hint--muted {
  margin-bottom: 0;
  opacity: 0.92;
}

.form-streaming .zlm-secret-hint code {
  font-size: 11px;
  padding: 0 4px;
  border-radius: 3px;
  background: var(--el-fill-color-light, #f5f7fa);
}

.form-streaming .field-footnote {
  margin: 6px 0 0;
  font-size: 12px;
  color: var(--gb-text-soft, #909399);
}

.streaming-extras {
  max-width: 600px;
  margin-top: 8px;
}

.streaming-extra-row {
  display: flex;
  align-items: flex-start;
}

.streaming-extra-label {
  flex: 0 0 120px;
  width: 120px;
  padding-right: 12px;
  box-sizing: border-box;
  text-align: right;
  line-height: 32px;
  font-size: 14px;
  color: var(--el-text-color-regular, #606266);
}

.streaming-extra-content {
  flex: 1;
  min-width: 0;
}

.streaming-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 18px;
  padding-left: 132px;
}

.hint {
  font-size: 12px;
  color: var(--gb-text-soft);
}

.form-item-hint {
  margin-left: 10px;
  font-size: 12px;
  color: var(--gb-text-soft);
}
</style>


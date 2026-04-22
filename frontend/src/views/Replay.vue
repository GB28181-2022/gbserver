<template>
  <div class="page replay">
    <header class="page-header">
      <div>
        <h2 class="page-title">录像回放</h2>
        <p class="page-subtitle">
          通过国标录像检索接口查询特定时间段的录制片段。
        </p>
      </div>
    </header>
    <el-card shadow="hover" class="search-card">
      <template #header>
        <span>录像检索条件（占位）</span>
      </template>
      <el-form :model="query" label-width="100px" inline>
        <el-form-item label="通道">
          <el-select v-model="query.channel" placeholder="选择通道">
            <el-option
              v-for="ch in channels"
              :key="ch.id"
              :label="ch.name"
              :value="ch.id"
            />
          </el-select>
        </el-form-item>
        <el-form-item label="时间范围">
          <el-date-picker
            v-model="query.range"
            type="datetimerange"
            range-separator="至"
            start-placeholder="开始时间"
            end-placeholder="结束时间"
          />
        </el-form-item>
        <el-form-item>
          <el-button type="primary" @click="onSearch">查询</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card shadow="hover">
      <template #header>
        <span>录像列表（示例数据）</span>
      </template>
      <el-table :data="records" style="width: 100%">
        <el-table-column prop="channelName" label="通道" width="160" />
        <el-table-column prop="start" label="开始时间" width="200" />
        <el-table-column prop="end" label="结束时间" width="200" />
        <el-table-column prop="duration" label="时长(秒)" width="100" />
        <el-table-column label="操作" width="160">
          <template #default>
            <el-button type="primary" size="small" disabled>回放</el-button>
            <el-button size="small" disabled>下载</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>
  </div>
</template>

<script setup>
import { reactive, ref } from 'vue'

const channels = [
  { id: 'camera-1', name: '摄像头 1' },
  { id: 'camera-2', name: '摄像头 2' },
]

const query = reactive({
  channel: channels[0].id,
  range: [],
})

const records = ref([
  {
    id: 1,
    channelId: 'camera-1',
    channelName: '摄像头 1',
    start: '2026-01-01 10:00:00',
    end: '2026-01-01 10:10:00',
    duration: 600,
  },
])

function onSearch() {
  // 占位：后续接入录像检索接口
  // eslint-disable-next-line no-console
  console.log('replay search (mock):', { ...query })
}
</script>

<style scoped>
.page {
  max-width: 1000px;
}

.page-header {
  margin-bottom: 16px;
}

.search-card {
  margin-bottom: 16px;
}
</style>


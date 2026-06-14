<script setup lang="ts">
import { useData } from 'vitepress'

const { site } = useData()
const base = site.value.base
const todoLink = `${base}todo/`

interface ProgressItem {
  label: string
  sub: string
  status: 'done' | 'active' | 'planned'
  text: string
}

const items: ProgressItem[] = [
  { label: '站点骨架', sub: 'VitePress + 章节目录 + 自动 sidebar', status: 'done', text: '已完成' },
  { label: 'D1 代码骨架', sub: 'POSIX / MSVC demo、Mock HAL + bridge', status: 'active', text: '进行中' },
  { label: 'D2 教程内容', sub: '15 章正文，对齐官方书', status: 'planned', text: '规划中' },
  { label: 'D3 差异化章节', sub: '仿真坑点 + RT-Thread 对比轨', status: 'planned', text: '规划中' },
  { label: 'D4 组件工程化', sub: '调度甘特图 + 版本 / CI / 自检', status: 'planned', text: '规划中' },
]
</script>

<template>
  <div class="home-progress">
    <div class="home-progress-inner">
      <div class="home-progress-header">
        <h2 class="home-progress-title">当前进度</h2>
        <p class="home-progress-desc">本教程正在持续建设，以下是各方向的实时状态</p>
      </div>

      <div class="progress-grid">
        <div
          v-for="it in items"
          :key="it.label"
          class="progress-card"
          :class="`progress-card--${it.status}`"
        >
          <div class="progress-card-head">
            <span class="progress-card-label">{{ it.label }}</span>
            <StatusTag :type="it.status" :text="it.text" />
          </div>
          <span class="progress-card-sub">{{ it.sub }}</span>
        </div>
      </div>

      <div class="home-progress-cta">
        <a :href="todoLink" class="progress-cta">
          <span>查看完整规划与任务清单</span>
          <span class="progress-cta-arrow">→</span>
        </a>
      </div>
    </div>
  </div>
</template>

<style scoped>
.home-progress {
  padding: 56px 24px 16px;
  overflow: visible;
}

.home-progress-inner {
  max-width: 920px;
  margin: 0 auto;
}

/* ── Header ── */
.home-progress-header {
  text-align: center;
  margin-bottom: 36px;
}

.home-progress-title {
  margin: 0;
  font-size: 28px;
  font-weight: 700;
  line-height: 1.5;
  background: linear-gradient(135deg, var(--vp-c-brand-1), var(--vp-c-indigo-1), var(--vp-c-purple-1));
  -webkit-background-clip: text;
  background-clip: text;
  -webkit-text-fill-color: transparent;
}

.home-progress-desc {
  margin: 10px 0 0;
  font-size: 15px;
  color: var(--vp-c-text-2);
  line-height: 1.7;
}

/* ── Grid ── */
.progress-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
  gap: 14px;
}

/* ── Card ── */
.progress-card {
  position: relative;
  padding: 18px 20px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 14px;
  background-color: var(--vp-c-bg);
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.04), 0 1px 2px rgba(0, 0, 0, 0.06);
  overflow: hidden;
  transition: border-color 0.35s ease, box-shadow 0.35s ease, transform 0.35s ease;
}

.progress-card::before {
  content: '';
  position: absolute;
  left: 0;
  top: 0;
  bottom: 0;
  width: 3px;
}

.progress-card--done::before { background: #53a253; }
.progress-card--active::before { background: var(--vp-c-brand-1); }
.progress-card--planned::before { background: var(--vp-c-divider); }

.progress-card:hover {
  border-color: var(--vp-c-brand-1);
  box-shadow: 0 10px 28px rgba(0, 0, 0, 0.1), 0 4px 8px rgba(0, 0, 0, 0.06);
  transform: translateY(-2px);
}

.progress-card-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  margin-bottom: 8px;
}

.progress-card-label {
  font-size: 15px;
  font-weight: 600;
  color: var(--vp-c-text-1);
  line-height: 1.4;
}

.progress-card-sub {
  font-size: 12.5px;
  color: var(--vp-c-text-2);
  line-height: 1.6;
}

/* ── CTA ── */
.home-progress-cta {
  display: flex;
  justify-content: center;
  margin-top: 36px;
}

.progress-cta {
  display: inline-flex;
  align-items: center;
  gap: 10px;
  padding: 13px 30px;
  border-radius: 14px;
  background: linear-gradient(135deg, var(--vp-c-brand-1), var(--vp-c-indigo-1));
  color: var(--vp-c-white);
  font-size: 15px;
  font-weight: 600;
  text-decoration: none !important;
  box-shadow: 0 4px 16px rgba(81, 107, 232, 0.25);
  transition: transform 0.35s ease, box-shadow 0.35s ease;
}

.progress-cta:hover {
  transform: translateY(-2px);
  box-shadow: 0 8px 28px rgba(81, 107, 232, 0.35);
}

.progress-cta-arrow {
  transition: transform 0.35s ease;
}

.progress-cta:hover .progress-cta-arrow {
  transform: translateX(4px);
}

/* ── Dark ── */
.dark .progress-card {
  background-color: var(--vp-c-bg-elv);
  border-color: var(--vp-c-border);
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.2), 0 1px 2px rgba(0, 0, 0, 0.15);
}

.dark .progress-card:hover {
  box-shadow: 0 10px 28px rgba(0, 0, 0, 0.3), 0 4px 8px rgba(0, 0, 0, 0.2);
}

/* ── Responsive ── */
@media (max-width: 639px) {
  .home-progress {
    padding: 40px 16px 8px;
  }

  .home-progress-title {
    font-size: 22px;
  }

  .progress-card {
    padding: 16px 16px;
  }
}
</style>

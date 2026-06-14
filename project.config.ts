import { defineProject } from './site/.vitepress/config/schema'

export default defineProject({
  name: 'tutorial_freertos',
  title: { 'zh-CN': 'Tutorial_FreeRTOS 的教程文档' },
  description: { 'zh-CN': '无硬件主机模拟的 FreeRTOS 教程——在 PC 上跑通《Mastering the FreeRTOS Kernel》全部示例' },
  base: '/Tutorial_FreeRTOS/',
  copyright: 'Copyright © 2026 Charliechen114514 - 保留所有权利',

  documentsDir: 'document',
  siteDir: 'site',

  locales: [
    { code: 'zh-CN', label: '中文', default: true },
  ],

  nav: {
    'zh-CN': [
      { text: '首页', link: '/' },
      { text: '教程', link: '/tutorial/' },
      { text: '仿真坑点', link: '/pitfalls/' },
      { text: 'RT-Thread 对比', link: '/rt-thread/' },
      { text: '规划', link: '/todo/' },
      { text: 'GitHub', link: 'https://github.com/Awesome-Embedded-Learning-Studio/Tutorial_FreeRTOS' },
    ],
  },

  sidebar: {
    volumes: [
      { name: 'tutorial', srcDir: 'tutorial', urlPrefix: '/tutorial' },
      { name: 'pitfalls', srcDir: 'pitfalls', urlPrefix: '/pitfalls' },
      { name: 'rt-thread', srcDir: 'rt-thread', urlPrefix: '/rt-thread' },
      { name: 'todo', srcDir: 'todo', urlPrefix: '/todo' },
    ],
  },

  github: {
    owner: 'Awesome-Embedded-Learning-Studio',
    repo: 'Tutorial_FreeRTOS',
    branch: 'main',
    documentsPath: 'document',
  },

  build: {
    concurrency: 4,
    rootPages: ['index.md'],
    rootAssets: [],
  },

  plugins: {
    cppTemplateEscape: true,
    kbd: true,
    math: true,
  },

  favicon: '/Tutorial_FreeRTOS/Awesome-Embedded.ico',

  homeBanner: {
    'zh-CN': '🚀 新手必读:本教程不依赖真 MCU,在 PC 上即可跑通 FreeRTOS 全部内核示例。先看 <a href="/Tutorial_FreeRTOS/tutorial/00_roadmap/">学习路线图</a> 了解双轨模拟结构与章节脉络。',
  },
})

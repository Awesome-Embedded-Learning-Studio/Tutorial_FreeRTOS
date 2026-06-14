import type { DefaultTheme } from 'vitepress'
import { readdirSync, statSync, readFileSync, existsSync } from 'fs'
import { join } from 'path'
import type { ProjectConfig, VolumeConfig } from './schema'

type SidebarItem = DefaultTheme.SidebarItem

function extractTitle(filePath: string): string | null {
  try {
    const content = readFileSync(filePath, 'utf-8')
    const fmMatch = content.match(/^---[\s\S]*?^title:\s*['"]?(.+?)['"]?\s*$/m)
    if (fmMatch) return fmMatch[1]
    const h1 = content.match(/^#\s+(.+)$/m)
    if (h1) return h1[1].replace(/\{.*?\}/g, '').trim()
  } catch { /* ignore */ }
  return null
}

function humanize(name: string): string {
  return name
    .replace(/^\d+[-]?/, '')
    .replace(/[-_]/g, ' ')
    .replace(/\b\w/g, c => c.toUpperCase())
}

// 学习顺序：路线图 → 为什么RTOS → 环境搭建 → 内存 → 任务 → 队列 → 定时器 → 中断(信号量) → 资源管理(互斥量) → 事件组 → 任务通知 → 低功耗 → 调试 → 排错 → 项目实战
// 注：实际排序优先按目录数字前缀(00_/01_/...)，本数组仅作无前缀时的兜底
// 对齐《Mastering the FreeRTOS Kernel》：内存在任务之前；信号量归中断章、互斥量归资源管理章
const LEARNING_ORDER = [
  'roadmap',
  'why-rtos',
  'environment',
  'memory',
  'tasks',
  'queues',
  'timers',
  'interrupts',
  'resources',
  'event-groups',
  'task-notifications',
  'low-power',
  'debugging',
  'troubleshooting',
  'project-dashboard',
]

function sortEntries(a: string, b: string): number {
  const na = a.match(/^(\d+)/)?.[1]
  const nb = b.match(/^(\d+)/)?.[1]
  if (na && nb) return parseInt(na) - parseInt(nb)
  if (na) return -1
  if (nb) return 1

  // 对于没有数字前缀的目录，按学习顺序排序
  const ia = LEARNING_ORDER.indexOf(a)
  const ib = LEARNING_ORDER.indexOf(b)
  if (ia !== -1 && ib !== -1) return ia - ib
  if (ia !== -1) return -1
  if (ib !== -1) return 1

  return a.localeCompare(b, 'en')
}

function scanDir(dir: string, urlPrefix: string, depth = 0): SidebarItem[] {
  if (depth > 5) return []

  let entries: string[]
  try {
    entries = readdirSync(dir).filter(e =>
      !e.startsWith('.') &&
      e !== 'hooks' &&
      e !== 'stylesheets' &&
      e !== 'javascripts' &&
      e !== 'images' &&
      e !== 'logo'
    )
  } catch { return [] }

  entries.sort(sortEntries)
  const items: SidebarItem[] = []

  for (const name of entries) {
    const fullPath = join(dir, name)
    if (!statSync(fullPath).isDirectory() && !name.endsWith('.md')) continue

    if (statSync(fullPath).isDirectory()) {
      const subItems = scanDir(fullPath, `${urlPrefix}/${name}`, depth + 1)
      const indexPath = join(fullPath, 'index.md')
      const title = extractTitle(indexPath) || humanize(name)

      if (subItems.length > 0) {
        items.push({
          text: title,
          link: existsSync(indexPath) ? `${urlPrefix}/${name}/` : undefined,
          items: subItems,
          collapsed: depth > 0,
        })
      } else if (existsSync(indexPath)) {
        items.push({ text: title, link: `${urlPrefix}/${name}/` })
      }
    } else if (name !== 'index.md' && name !== 'tags.md') {
      const title = extractTitle(fullPath) || humanize(name.replace(/\.md$/, ''))
      items.push({ text: title, link: `${urlPrefix}/${name.replace(/\.md$/, '')}` })
    }
  }

  return items
}

export function volumeSidebar(
  docsRoot: string,
  vol: VolumeConfig
): DefaultTheme.SidebarItem[] {
  const dir = join(docsRoot, vol.srcDir)
  const indexPath = join(dir, 'index.md')
  const items = scanDir(dir, vol.urlPrefix)

  const overviewTitle = extractTitle(indexPath) || humanize(vol.srcDir)
  return [
    { text: overviewTitle, link: `${vol.urlPrefix}/` },
    ...items,
  ]
}

export function buildSidebar(
  docsRoot: string,
  config: ProjectConfig
): DefaultTheme.Sidebar {
  const sidebar: DefaultTheme.Sidebar = {}

  for (const vol of config.sidebar.volumes) {
    sidebar[`${vol.urlPrefix}/`] = volumeSidebar(docsRoot, vol)
  }

  if (config.sidebar.extra) {
    Object.assign(sidebar, config.sidebar.extra)
  }

  // Build sidebar for non-default locales
  for (const locale of config.locales) {
    if (locale.default || !locale.dir) continue
    const localeDir = join(docsRoot, locale.dir)
    if (!existsSync(localeDir)) continue

    const localeItems = scanDir(localeDir, locale.prefix || `/${locale.dir}`)
    if (localeItems.length > 0) {
      const prefix = locale.prefix || `/${locale.dir}/`
      sidebar[prefix] = [{ text: locale.label, items: localeItems }]
    }
  }

  return sidebar
}

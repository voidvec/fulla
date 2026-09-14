# Playwright E2E 自动化测试接入指南

> 基于 OAuth2 Admin 项目实践总结，面向其他前端项目的 Playwright E2E 测试接入参考。

---

## 目录

1. [核心原理](#1-核心原理)
2. [项目初始化](#2-项目初始化)
3. [Mock API 层设计](#3-mock-api-层设计)
4. [测试文件组织](#4-测试文件组织)
5. [测试编写模式](#5-测试编写模式)
6. [进阶技巧](#6-进阶技巧)
7. [CI/CD 集成](#7-cicd-集成)
8. [常见问题](#8-常见问题)

---

## 1. 核心原理

### 1.1 为什么选择请求拦截而不是真实后端

| 方案 | 优点 | 缺点 |
|------|------|------|
| **请求拦截 Mock** | 无需后端依赖、执行快(&lt;5s)、稳定不 flaky、可自由控制响应 | 不验证前后端集成 |
| **真实后端** | 验证端到端集成 | 需要数据库/缓存/服务、慢(分钟级)、环境依赖多、数据隔离难 |
| **MSW (Mock Service Worker)** | 浏览器层拦截、更贴近真实 | 配置复杂、需要 Service Worker 支持 |

本项目选择 **Playwright 原生 `page.route()` 请求拦截**，理由：
- 零额外依赖（Playwright 内置）
- API 简洁直观
- 拦截发生在网络层之前，性能最好
- 支持精确匹配 URL 模式和 HTTP 方法
- 支持在单个测试中覆盖全局 Mock

### 1.2 请求拦截工作流程

```
┌──────────────┐      HTTP 请求       ┌──────────────────┐
│  前端应用代码  │ ───────────────────→ │  page.route()    │
│  (浏览器)     │                      │  URL 模式匹配     │
│              │ ←─────────────────── │  route.fulfill() │
└──────────────┘     Mock 响应         └──────────────────┘
                                          ↑ 不经过网络层
                                     (无真实 HTTP 连接)
```

Playwright 的 `page.route()` 在浏览器网络层拦截请求，**请求不会离开浏览器进程**。这意味着：
- 不需要启动后端服务
- 不需要网络连接
- 响应是即时的，无延迟
- 测试完全确定性，无网络 flaky

### 1.3 三层架构

```
tests/e2e/
├── helpers/
│   └── mock-api.ts          ← Layer 1: Mock 数据 + 拦截器
├── auth.spec.ts             ← Layer 2: 测试用例
├── applications.spec.ts
└── ...

playwright.config.ts         ← Layer 3: Playwright 配置
```

| 层次 | 职责 | 修改频率 |
|------|------|---------|
| **Mock 层** | 定义 Mock 数据常量 + `setupAuthenticatedMocks()` 全局拦截函数 | 后端 API 变更时 |
| **测试层** | 编写具体测试用例，调用 Mock 层提供的函数 | 新功能/新页面时 |
| **配置层** | Playwright 运行参数、浏览器、webServer | 项目初始化时 |

---

## 2. 项目初始化

### 2.1 安装依赖

```bash
npm install -D @playwright/test
npx playwright install chromium
```

仅需 Chromium，无需安装 WebKit/Firefox。E2E 测试的目标是验证功能逻辑，不是跨浏览器兼容性。

### 2.2 Playwright 配置

创建 `playwright.config.ts`：

```typescript
import { defineConfig, devices } from '@playwright/test'

export default defineConfig({
  testDir: './tests/e2e',        // 测试文件目录
  fullyParallel: true,            // 全并行执行（测试间无依赖时开启）
  forbidOnly: !!process.env.CI,   // CI 禁止 test.only（防止误提交）
  retries: process.env.CI ? 2 : 0, // CI 重试 2 次（抗 flaky）
  workers: process.env.CI ? 1 : undefined, // CI 单 worker（避免资源竞争）
  reporter: 'html',               // HTML 测试报告

  use: {
    baseURL: 'http://localhost:5174',  // 应用基础 URL（配合 page.goto() 使用）
    trace: 'on-first-retry',           // 失败重试时记录 trace（调试用）
  },

  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],

  webServer: {
    command: 'npm run dev',              // 自动启动 dev server
    url: 'http://localhost:5174/',       // 等待此 URL 可访问
    reuseExistingServer: !process.env.CI, // 本地复用已运行的 server
    timeout: 30000,                      // 启动超时 30s
  },
})
```

**关键配置说明：**

| 配置项 | 作用 | 推荐值 |
|--------|------|--------|
| `baseURL` | `page.goto('/path')` 时自动拼接此前缀 | 开发服务器地址 |
| `webServer` | 自动启动/复用 dev server | 开发模式 + CI 模式区分 |
| `trace` | 失败时生成 trace 文件，可用 `npx playwright show-trace` 调试 | `on-first-retry` |
| `fullyParallel` | 多个 test 文件并行执行 | `true`（Mock 模式下安全） |
| `retries` | 失败重试次数 | CI: 2, 本地: 0 |

### 2.3 package.json 脚本

```json
{
  "scripts": {
    "test:e2e": "playwright test",
    "test:e2e:headed": "playwright test --headed",
    "test:e2e:ui": "playwright test --ui"
  }
}
```

### 2.4 目录结构

```
your-project/
├── playwright.config.ts
├── package.json
├── src/                        ← 应用源码
└── tests/
    └── e2e/
        ├── helpers/
        │   └── mock-api.ts     ← Mock 数据 + 拦截函数
        ├── auth.spec.ts        ← 认证相关测试
        ├── page-a.spec.ts      ← 页面 A 测试
        └── page-b.spec.ts      ← 页面 B 测试
```

---

## 3. Mock API 层设计

Mock API 层是整个测试体系的**核心**。设计好这一层，编写测试用例会非常简单。

### 3.1 文件结构

`tests/e2e/helpers/mock-api.ts` 分为三个部分：

```
Part 1: Mock 数据常量    →  定义所有 API 的假数据
Part 2: setupAuthenticatedMocks()  →  注册全局路由拦截
Part 3: 辅助函数         →  loginAsAdmin() 等常用操作
```

### 3.2 Mock 数据常量

**原则：数据尽量真实，字段与后端 API 响应一致。**

```typescript
// ✅ 好的设计：字段名、类型与真实 API 一致
export const MOCK_USERS = [
  {
    id: '550e8400-e29b-41d4-a716-446655440000',
    username: 'admin',
    email: 'admin@example.com',
    email_verified: true,    // boolean，不是字符串
    mfa_enabled: true,
  },
  {
    id: '660e8400-e29b-41d4-a716-446655440001',
    username: 'testuser',
    email: 'test@example.com',
    email_verified: false,
    mfa_enabled: false,
  },
]

// ❌ 不好的设计：字段名随意、数据不真实
export const users = [
  { uid: 1, name: 'a', mail: 'a@b' },  // 字段名不匹配真实 API
]
```

**为什么需要多组数据？**

Mock 数据至少准备两种状态，覆盖不同的 UI 表现：
- `email_verified: true` + `false` → 测试"已验证"和"待验证"Badge
- `mfa_enabled: true` + `false` → 测试"已开启"和"未开启"Badge

### 3.3 路由拦截：setupAuthenticatedMocks()

这是最关键的函数，负责拦截前端发出的所有 API 请求。

```typescript
import { Page } from '@playwright/test'

export async function setupAuthenticatedMocks(page: Page) {
  // 拦截规则：** 是通配符，匹配任何 origin
  await page.route('**/api/users', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ users: MOCK_USERS }),
    })
  })
}
```

**URL 匹配模式说明：**

| 模式 | 匹配范围 | 示例 |
|------|---------|------|
| `**/api/users` | 任何 origin + 路径精确匹配 | `http://localhost:5174/api/users` ✅ |
| `**/api/admin/logs**` | 路径前缀匹配（含查询参数） | `/api/admin/logs?page=2` ✅ |
| `**/api/admin/clients/*` | 路径 + 单段通配 | `/api/admin/clients/fulla-portal` ✅ |
| `**/api/admin/clients/*/reset-secret` | 多段路径混合 | `/api/admin/clients/fulla-portal/reset-secret` ✅ |

**同一个 URL，不同 HTTP 方法：**

```typescript
await page.route('**/api/admin/clients', async (route) => {
  if (route.request().method() === 'GET') {
    await route.fulfill({ status: 200, body: JSON.stringify({ clients: MOCK_CLIENTS }) })
  } else if (route.request().method() === 'POST') {
    await route.fulfill({ status: 201, body: JSON.stringify({ client_id: 'new-123' }) })
  } else {
    // 未预期的方法，交给下一个 handler 或真实网络
    await route.continue()
  }
})
```

**子资源路由优先级：**

Playwright 路由匹配按注册顺序，**更具体的路由应先注册**：

```typescript
// ✅ 正确：更具体的路由先注册
await page.route('**/api/admin/clients/*/reset-secret', ...)  // 先匹配
await page.route('**/api/admin/clients/*', ...)                // 后匹配（兜底）

// 实际上 Playwright 的通配符匹配有隐式优先级
// 但在 handler 内主动判断更安全：
await page.route('**/api/admin/clients/*', async (route) => {
  const url = route.request().url()
  if (url.includes('/scopes') || url.includes('/reset-secret')) {
    await route.continue()  // 跳过，让更具体的 handler 处理
    return
  }
  // ... 处理 DELETE / GET / PUT
})
```

### 3.4 辅助函数：loginAsAdmin()

```typescript
export async function loginAsAdmin(page: Page) {
  await page.goto('/login')
  await page.fill('input[type="text"]', 'admin')
  await page.fill('input[type="password"]', 'admin')
  await page.click('button[type="submit"]')
  await page.waitForURL('**/dashboard')  // 等待登录成功跳转
}
```

**设计要点：**
- 通过 UI 操作完成登录（模拟真实用户行为）
- 依赖 `setupAuthenticatedMocks()` 已拦截认证 API
- `waitForURL` 确保登录完成，后续测试处于已认证状态

### 3.5 适配其他项目的 Mock 模板

```typescript
// === helpers/mock-api.ts 模板 ===

import { Page } from '@playwright/test'

// ---- Part 1: Mock 数据 ----

export const CURRENT_USER = {
  id: '1',
  name: 'Test User',
  email: 'test@example.com',
  role: 'admin',
}

export const MOCK_ITEMS = [
  { id: '1', title: 'Item A', status: 'active' },
  { id: '2', title: 'Item B', status: 'inactive' },
]

// ---- Part 2: 全局路由拦截 ----

export async function setupAuthenticatedMocks(page: Page) {
  // 认证相关
  await page.route('**/auth/login', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ token: 'mock-jwt-token', user: CURRENT_USER }),
    })
  })

  await page.route('**/auth/me', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(CURRENT_USER),
    })
  })

  // 业务数据
  await page.route('**/api/items', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ items: MOCK_ITEMS }),
      })
    } else if (route.request().method() === 'POST') {
      const body = JSON.parse(route.request().postData() || '{}')
      await route.fulfill({
        status: 201,
        contentType: 'application/json',
        body: JSON.stringify({ id: 'new-' + Date.now(), ...body }),
      })
    } else {
      await route.continue()
    }
  })

  // 单项操作（GET / PUT / DELETE）
  await page.route('**/api/items/*', async (route) => {
    if (route.request().method() === 'DELETE') {
      await route.fulfill({ status: 204 })
    } else {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify(MOCK_ITEMS[0]),
      })
    }
  })
}

// ---- Part 3: 辅助函数 ----

export async function loginAs(page: Page, username = 'admin', password = 'admin') {
  await page.goto('/login')
  await page.fill('[name="username"]', username)
  await page.fill('[name="password"]', password)
  await page.click('button[type="submit"]')
  await page.waitForURL('**/dashboard')
}
```

---

## 4. 测试文件组织

### 4.1 命名规范

```
tests/e2e/
  ├── helpers/
  │   └── mock-api.ts         ← 固定命名，所有测试共享
  ├── auth.spec.ts             ← 认证/登录相关
  ├── {page-name}.spec.ts      ← 每个页面一个文件
  └── navigation.spec.ts       ← 全局导航/布局
```

**一个页面 = 一个 spec 文件**，按功能域划分，不按操作类型划分。

### 4.2 test.describe 分组

```typescript
test.describe('页面/功能名称', () => {
  // beforeEach: 统一 setup
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)
    await loginAsAdmin(page)
    // 导航到目标页面
    await page.click('nav a:has-text("Target Page")')
    await page.waitForURL('**/target-page')
  })

  // 测试用例按：渲染 → 数据 → 交互 → 边界 排列
  test('displays page title', ...)
  test('shows data from API', ...)
  test('button click triggers action', ...)
  test('shows error on failure', ...)
})
```

### 4.3 测试用例命名

使用**陈述句**描述预期行为，而非操作步骤：

```typescript
// ✅ 好：描述预期结果
test('displays users list with correct columns', ...)
test('shows error on login failure', ...)
test('delete button removes item from list', ...)

// ❌ 不好：描述操作步骤
test('clicks button and checks result', ...)
test('test 1', ...)
```

---

## 5. 测试编写模式

### 5.1 标准测试流程（beforeEach 模式）

**最常用的模式**，90% 的测试都遵循这个流程：

```typescript
test.describe('User Management', () => {
  test.beforeEach(async ({ page }) => {
    // Step 1: 注册全局 Mock
    await setupAuthenticatedMocks(page)
    // Step 2: 模拟登录
    await loginAsAdmin(page)
    // Step 3: 导航到目标页面
    await page.click('nav a:has-text("Users")')
    await page.waitForURL('**/users')
  })

  test('displays user table', async ({ page }) => {
    await expect(page.locator('h2')).toContainText('Users')
    await expect(page.locator('th:has-text("Username")')).toBeVisible()
  })
})
```

**流程图：**

```
beforeEach:
  setupAuthenticatedMocks(page)
       ↓
  loginAsAdmin(page)
       ↓
  导航到目标页面 + waitForURL
       ↓
  ═══════════════════════════
  ↓  test case 1 执行      ↓
  ↓  test case 2 执行      ↓
  ↓  ...                   ↓
  ═══════════════════════════
```

### 5.2 页面渲染验证

验证页面正确显示数据：

```typescript
test('displays users list with correct columns', async ({ page }) => {
  // 验证标题
  await expect(page.locator('h2')).toContainText('Users')
  // 验证表头
  await expect(page.locator('th:has-text("Username")')).toBeVisible()
  await expect(page.locator('th:has-text("Email")')).toBeVisible()
  // 验证数据行（来自 Mock 数据）
  const tableBody = page.locator('tbody')
  await expect(tableBody.getByRole('cell', { name: 'admin', exact: true })).toBeVisible()
})
```

### 5.3 表单交互测试

验证表单填写、提交、响应：

```typescript
test('creates a new application and shows secret', async ({ page }) => {
  // 1. 触发弹窗
  await page.click('button:has-text("Create Application")')
  // 2. 验证弹窗出现
  await expect(page.locator('h3:has-text("Create Application")')).toBeVisible()
  // 3. 填写表单
  await page.fill('input[placeholder="My App"]', 'Test Application')
  await page.selectOption('select', 'CONFIDENTIAL')
  // 4. 提交
  await page.locator('.fixed button[type="submit"]').click()
  // 5. 验证结果
  await expect(page.locator('h3:has-text("Client Secret")')).toBeVisible()
  await expect(page.locator('.font-mono.select-all')).toContainText('generated-secret-abc123xyz')
})
```

### 5.4 Modal/对话框测试

```typescript
test('opens and closes role assignment modal', async ({ page }) => {
  // 打开
  await page.click('button:has-text("Assign Roles")')
  await expect(page.locator('h3:has-text("Assign Roles")')).toBeVisible()

  // 关闭
  await page.click('button:has-text("Cancel")')
  await expect(page.locator('h3:has-text("Assign Roles")')).not.toBeVisible()
})

// 原生 confirm 对话框处理
test('delete with confirmation', async ({ page }) => {
  page.on('dialog', (dialog) => dialog.accept())  // 自动接受 confirm
  await page.click('button:has-text("Delete")')
  await expect(page.locator('h2')).toContainText('Applications')
})
```

### 5.5 分页测试

```typescript
test('pagination sends correct page parameter', async ({ page }) => {
  // 构造足够多的数据以启用"下一页"
  const manyItems = Array.from({ length: 50 }, (_, i) => ({
    id: i + 1, title: `Item ${i}`, status: 'active',
  }))

  let requestedPage = 1
  await page.route('**/api/items**', async (route) => {
    const url = new URL(route.request().url())
    requestedPage = parseInt(url.searchParams.get('page') || '1')
    await route.fulfill({
      status: 200,
      body: JSON.stringify({ items: requestedPage === 1 ? manyItems : MOCK_ITEMS }),
    })
  })

  // 导航触发重新加载
  await page.click('nav a:has-text("Dashboard")')
  await page.click('nav a:has-text("Items")')
  await page.waitForURL('**/items')

  // 点击下一页
  const nextBtn = page.locator('button:has-text("Next")')
  await expect(nextBtn).not.toBeDisabled()
  await nextBtn.click()
  await expect(page.locator('text=Page 2')).toBeVisible()
  expect(requestedPage).toBe(2)
})
```

### 5.6 导航测试

```typescript
test('sidebar navigation works for all pages', async ({ page }) => {
  // 验证导航项可见
  await expect(page.locator('nav a:has-text("Dashboard")')).toBeVisible()
  await expect(page.locator('nav a:has-text("Users")')).toBeVisible()

  // 点击并验证 URL + 页面标题
  await page.click('nav a:has-text("Users")')
  await expect(page).toHaveURL(/\/users/)
  await expect(page.locator('h2')).toContainText('Users')

  // 返回验证
  await page.click('nav a:has-text("Dashboard")')
  await expect(page).toHaveURL(/\/dashboard/)
})
```

---

## 6. 进阶技巧

### 6.1 覆盖全局 Mock（测试异常场景）

这是本方案最强大的特性：**在单个测试中覆盖全局 Mock，无需修改 setupAuthenticatedMocks()**。

**原理：** `page.route()` 后注册的 handler 优先匹配。后注册的会覆盖先注册的同 URL 模式。

```typescript
test.describe('Dashboard', () => {
  test.beforeEach(async ({ page }) => {
    await setupAuthenticatedMocks(page)  // 全局 Mock：health 返回 ok
    await loginAsAdmin(page)
  })

  test('displays healthy status', async ({ page }) => {
    await expect(page.locator('text=Healthy')).toBeVisible()  // 使用全局 Mock
  })

  test('shows unhealthy status when backend is down', async ({ page }) => {
    // 关键：在全局 Mock 之后注册，覆盖全局的 health 响应
    await page.route('**/health/ready', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'error', message: 'Database unreachable' }),
      })
    })

    await loginAsAdmin(page)  // 重新登录以触发 health check
    await expect(page.locator('text=Unhealthy')).toBeVisible()
  })
})
```

**常见覆盖场景：**

```typescript
// 场景 1：API 返回错误
await page.route('**/oauth2/login', async (route) => {
  await route.fulfill({ status: 401, body: JSON.stringify({ error: 'invalid_credentials' }) })
})

// 场景 2：API 返回空数据
await page.route('**/api/admin/clients', async (route) => {
  if (route.request().method() === 'GET') {
    await route.fulfill({ status: 200, body: JSON.stringify({ clients: [] }) })
  } else {
    await route.continue()
  }
})

// 场景 3：非管理员用户
await page.route('**/oauth2/userinfo', async (route) => {
  await route.fulfill({
    status: 200,
    body: JSON.stringify({ sub: '123', username: 'user', roles: ['user'] }),
  })
})

// 场景 4：MFA 要求
await page.route('**/oauth2/login', async (route) => {
  await route.fulfill({
    status: 200,
    body: JSON.stringify({ mfa_required: true, mfa_token: 'mfa-token-123' }),
  })
})
```

### 6.2 捕获请求体验证

验证前端发送的请求参数是否正确：

```typescript
test('assigns roles with correct request body', async ({ page }) => {
  let requestBody: any = null

  // 覆盖全局 Mock，同时捕获请求体
  await page.route('**/api/admin/users/*/roles', async (route) => {
    requestBody = JSON.parse(route.request().postData() || '{}')
    await route.fulfill({
      status: 200,
      body: JSON.stringify({ message: 'Roles updated' }),
    })
  })

  // 执行操作
  await page.locator('button:has-text("Assign Roles")').first().click()
  await page.fill('input[placeholder="admin, user"]', 'admin, editor')
  await page.click('button:has-text("Save Roles")')

  // 验证请求体
  expect(requestBody).toEqual({ roles: ['admin', 'editor'] })
})
```

### 6.3 空状态测试

验证列表为空时的 UI 表现：

```typescript
test('shows empty state when no items exist', async ({ page }) => {
  // 覆盖 Mock 返回空列表
  await page.route('**/api/items', async (route) => {
    if (route.request().method() === 'GET') {
      await route.fulfill({ status: 200, body: JSON.stringify({ items: [] }) })
    } else {
      await route.continue()
    }
  })

  // 导航走再回来，触发数据重新加载
  await page.click('nav a:has-text("Dashboard")')
  await page.click('nav a:has-text("Items")')
  await page.waitForURL('**/items')

  // 验证空状态提示
  await expect(page.locator('text=No items yet')).toBeVisible()
  await expect(page.locator('button:has-text("Create your first item")')).toBeVisible()
})
```

**为什么需要"导航走再回来"？**

因为 `beforeEach` 中已经导航到了目标页面，数据已经加载。覆盖 Mock 后需要触发组件重新挂载数据重新获取。两个方法：
1. 导航到其他页面再回来（推荐，模拟真实用户操作）
2. `await page.reload()`（简单，但有些组件可能不重新请求）

### 6.4 验证请求查询参数

```typescript
test('filter sends correct parameters', async ({ page }) => {
  let capturedUrl = ''
  await page.route('**/api/items**', async (route) => {
    capturedUrl = route.request().url()
    await route.fulfill({ status: 200, body: JSON.stringify({ items: [] }) })
  })

  // 执行筛选操作
  await page.selectOption('select[name="status"]', 'active')
  await page.click('button:has-text("Filter")')

  // 验证 URL 参数
  const url = new URL(capturedUrl)
  expect(url.searchParams.get('status')).toBe('active')
})
```

---

## 7. CI/CD 集成

### 7.1 GitHub Actions 示例

```yaml
name: E2E Tests

on: [push, pull_request]

jobs:
  e2e:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with:
          node-version: 20

      - name: Install dependencies
        run: npm ci

      - name: Install Playwright browsers
        run: npx playwright install chromium --with-deps

      - name: Run E2E tests
        run: npm run test:e2e

      - name: Upload test report
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: playwright-report
          path: playwright-report/
```

### 7.2 CI 配置要点

| 配置 | CI 值 | 本地值 | 原因 |
|------|-------|-------|------|
| `workers` | 1 | 自动(多) | CI 资源有限，避免竞争 |
| `retries` | 2 | 0 | CI 网络不稳定，重试抗 flaky |
| `reuseExistingServer` | `false` | `true` | CI 必须启动新 server |
| `forbidOnly` | `true` | `false` | 防止 `test.only` 被提交 |

---

## 8. 常见问题

### Q1: 测试偶尔失败（flaky）怎么办？

1. 检查是否用了 `page.waitForTimeout()` — 改用 `waitForSelector` / `waitForURL` / `expect().toBeVisible()`
2. 检查 Mock 是否有条件竞争 — 确保所有 API 都被拦截，没有未 Mock 的请求
3. 开启 `trace: 'on-first-retry'`，用 `npx playwright show-trace` 分析失败

### Q2: 某个请求没被拦截怎么办？

- 检查 URL 模式是否匹配（`**` 通配符范围）
- 打开浏览器开发者工具，看实际请求的完整 URL
- 在 handler 内加 `console.log(route.request().url())` 调试

### Q3: 如何测试文件上传？

```typescript
// 监听 file chooser 事件
const [fileChooser] = await Promise.all([
  page.waitForEvent('filechooser'),
  page.click('button:has-text("Upload")'),  // 触发文件选择
])
await fileChooser.setFiles({
  name: 'test.csv',
  mimeType: 'text/csv',
  buffer: Buffer.from('name,value\ntest,123'),
})
```

### Q4: 如何在不启动 dev server 的情况下测试？

修改 `playwright.config.ts`，移除 `webServer` 配置，手动启动 dev server 后运行测试：

```bash
# 终端 1
npm run dev

# 终端 2
npm run test:e2e
```

### Q5: Mock 模式和真实后端测试如何共存？

```
tests/
├── e2e/
│   ├── helpers/
│   │   ├── mock-api.ts      ← Mock 模式
│   │   └── api-client.ts    ← 真实 API 调用
│   ├── auth.spec.ts          ← Mock 测试
│   └── ...
└── integration/
    └── full-flow.spec.ts     ← 真实后端集成测试
```

使用不同的 `playwright.config.ts`：
- `playwright.config.ts` — Mock 模式（日常开发、每次提交）
- `playwright.integration.config.ts` — 真实后端（发布前、 nightly）

---

## 9. 后端集成测试故障排查

### 问题：测试脚本第二次运行时全部失败

**症状**：
```
POST http://localhost:5174/oauth2/login 401 (Unauthorized)
```

后端日志：
```
WARN  Account locked for user: admin until 1779441748
INFO  [METRIC] oauth2_login_failures_total reason=bad_credentials
```

**原因**：
OAuth2 系统实现了账号锁定机制。当登录失败次数达到阈值时，账号会被临时锁定：
- 5次失败 → 锁定1分钟
- 10次失败 → 锁定5分钟
- 15次失败 → 锁定30分钟
- 20次以上 → 锁定1小时

**解决方案**：

#### 方案1：使用自动清理的测试脚本

后端测试脚本（如 `test-admin-endpoints.ps1`）已经在结束时自动重置账号锁定状态。

对于本地PostgreSQL数据库，需要配置数据库密码：

```powershell
# 编辑测试脚本，找到清理部分，设置密码
$env:PGPASSWORD = "your_password"  # 修改为实际密码
```

#### 方案2：手动重置账号锁定

```powershell
# 使用重置脚本
$env:PGPASSWORD = "your_password"
.\scripts\backend\reset-account-lockout.ps1
$env:PGPASSWORD = $null

# 或直接使用SQL
psql -U fulla_user -d fulla_db -h localhost -c "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username='admin';"
```

#### 方案3：等待锁定自动解除

根据失败次数，等待相应的时间后账号会自动解锁。

**预防措施**：

1. **使用专用测试账号**：不要在测试中使用生产admin账号
2. **确保凭证正确**：检查测试脚本中的用户名和密码
3. **测试后自动清理**：在测试脚本末尾添加清理代码

详细说明请参考：[账号锁定机制](operate/account-lockout.md)

---

## 附录 A: 从零接入检查清单

将此清单用于新项目的 E2E 测试接入：

- [ ] `npm install -D @playwright/test`
- [ ] `npx playwright install chromium`
- [ ] 创建 `playwright.config.ts`
- [ ] 创建 `tests/e2e/helpers/mock-api.ts`
- [ ] 定义 Mock 数据常量（与后端 API 响应结构一致）
- [ ] 实现 `setupAuthenticatedMocks(page)` — 拦截所有认证 + 业务 API
- [ ] 实现 `loginAsAdmin(page)` — UI 登录辅助函数
- [ ] 创建第一个测试文件（建议从 auth 开始）
- [ ] 添加 `package.json` 脚本
- [ ] 配置 CI pipeline

## 附录 B: Admin 前端 E2E 测试统计

| 指标 | 数值 |
|------|------|
| 测试文件 | 16 |
| 测试用例 | 174 |
| 执行时间 | ~1 分钟 |
| 后端依赖 | 无（全 Mock） |
| 浏览器 | Chromium |
| 并行执行 | 是 |
| Mock API 端点 | 15+ |

---

> 本文档基于 fulla Admin 前端项目实践总结。项目源码：`frontends/admin/tests/e2e/`

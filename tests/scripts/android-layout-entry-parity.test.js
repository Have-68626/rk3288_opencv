// 布局入口一致性守卫（★ 2026-09-28 新增，出自外部审查 [P1]「Android 横屏缺少关键管理入口」的修复）。
//
// 背景：横屏布局**从未**包含模型导入/切换、模型状态、未决替换了结与人员管理入口
// （`git log -S` 可见这三个 id 在 layout-land 里从未出现过），而 `MainActivity` 在找不到控件时
// 走 `if (x != null) x.setOnClickListener(...)` ⇒ **静默跳过绑定** ⇒ 横屏用户无法完成这些操作，
// 且没有任何测试会红。而横屏很可能是 rk3288 上的**默认**形态（HDMI 常横屏冷启动、manifest 未锁定
// screenOrientation）⇒ 首次可用界面即残缺。
//
// 本守卫把「必须在两份布局里都存在」的功能入口 id 钉住：日后再分叉，这里先红而不是静默退化。
//
// ⚠️ 刻意是**白名单**而不是「portrait ⊆ landscape」：横屏缺 `bottom_nav` / `switch_accel_*` 等
//    是既有的（有意的）布局差异，全量包含断言会**恒红**、从而训练人忽略本测试。
// 判别力：从 layout-land 删掉 REQUIRED_IN_BOTH 里任一 id ⇒ 本用例红。

const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')

const ROOT = path.resolve(__dirname, '..', '..')
const RES = path.join(ROOT, 'app/src/main/res')
const LAYOUT_DIRS = ['layout', 'layout-land'].map((d) => path.join(RES, d))
const LAYOUT_PORT = path.join(RES, 'layout/activity_main.xml')
const LAYOUT_LAND = path.join(RES, 'layout-land/activity_main.xml')
const JAVA_SRC = path.join(ROOT, 'src/java')

function idsInFile(file) {
  const xml = fs.readFileSync(file, 'utf8')
  return new Set(
    [...xml.matchAll(/android:id="@\+id\/([A-Za-z0-9_]+)"/g)].map((m) => m[1]),
  )
}

function idsInAllLayouts() {
  const out = new Set()
  for (const dir of LAYOUT_DIRS) {
    for (const f of fs.readdirSync(dir)) {
      if (!f.endsWith('.xml')) continue
      for (const id of idsInFile(path.join(dir, f))) out.add(id)
    }
  }
  return out
}

// ⚠️ **必须扫全部 Java 源**，不能只看 MainActivity：控件绑定已按域拆分（例如 `btn_view_logs`
//    与 `btn_exit` 绑在 `MainScreenBinder`，`MainActivity` 里**没有任何引用**）。
//    ⇒ 本文件初版只扫 MainActivity，把这两个**已接线**的入口误报成「未接线」并让用例变红 ——
//    这正说明「守卫的前提也要被核实」，故此处收敛为全量扫描，并在下方加了自证诚实的断言。
function javaFiles() {
  const out = []
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name)
      if (e.isDirectory()) walk(p)
      else if (e.name.endsWith('.java')) out.push(p)
    }
  }
  walk(JAVA_SRC)
  return out
}

function allJavaText() {
  return javaFiles()
    .map((f) => fs.readFileSync(f, 'utf8'))
    .join('\n')
}

// 必须在**两份 activity_main 布局**里都存在的**功能入口**（按钮类：点下去要有行为）。
const REQUIRED_IN_BOTH = [
  'btn_register_face',
  'btn_view_logs',
  'btn_exit',
  'btn_import_model',
  'btn_switch_model',
  'btn_manage_persons',
  'btn_resolve_swap',
]

// 必须在两份布局里都存在的**状态展示面**（不是功能入口）。
// ⚠️ 刻意与上面分开（对抗性核查指出）：`tv_model_management` 只是个 `TextView`（显示模型状态文字），
//    缺了只丢一条状态、**不会**「功能不可达」。初版把它混进「功能入口」白名单，使本用例的**主张
//    比它实际检查的更宽** —— 若将来有人有意在横屏去掉该状态区，会**误红**。
//    这里只断言「两份布局都有」，并**不**断言它被绑定（它本就无需监听器）。
const REQUIRED_STATUS_IN_BOTH = ['tv_model_management']

test('布局入口一致性：功能入口在竖屏与横屏两份布局里都存在', () => {
  const port = idsInFile(LAYOUT_PORT)
  const land = idsInFile(LAYOUT_LAND)
  for (const id of [...REQUIRED_IN_BOTH, ...REQUIRED_STATUS_IN_BOTH]) {
    assert.ok(port.has(id), `竖屏布局缺少功能入口 ${id}`)
    assert.ok(
      land.has(id),
      `横屏布局缺少功能入口 ${id} —— 横屏下 MainActivity 会静默跳过绑定（见本文件头说明）`,
    )
  }
})

test('布局入口一致性：这组入口在 Java 侧确实被引用（**弱判据**，见下方说明）', () => {
  const src = allJavaText()
  // ⚠️ **本判据刻意弱**（对抗性核查指出）：「`src.includes('R.id.'+id)`」只证明**文本引用存在** ——
  //    在注释里写一句 `R.id.btn_exit`、或只 `findViewById` 却从不注册监听器，都能通过。
  //    真实反例就在本仓：`tv_model_management` 只有 `findViewById` 与 `setText`、**无任何监听器**。
  //    ⇒ 故它**不**声称「已接线」，只声称「有引用」；「按钮是否真的挂了监听器」需要更强的静态分析
  //    （或运行期测试），**不在本守卫的范围**。上面的 `REQUIRED_STATUS_IN_BOTH` 已按此原则拆出。
  for (const id of REQUIRED_IN_BOTH) {
    assert.ok(
      src.includes(`R.id.${id}`),
      `没有任何 Java 源引用 ${id}（控件存在却零引用 ⇒ 功能不可达）`,
    )
  }
})

test('布局入口一致性：MainActivity 的 findViewById(R.id.*) 无悬空引用', () => {
  const src = allJavaText()
  const referenced = new Set(
    [...src.matchAll(/findViewById\(R\.id\.([A-Za-z0-9_]+)\)/g)].map((m) => m[1]),
  )
  const all = idsInAllLayouts()
  const dangling = [...referenced].filter((id) => !all.has(id))
  assert.deepEqual(
    dangling,
    [],
    `有 Java 源绑定了任何布局里都不存在的 id：${dangling.join(', ')}`,
  )
})

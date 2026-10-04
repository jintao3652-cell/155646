/* i18n 初始化：默认英语，语言选择保存在 config.json（settings.language）。
 * 支持 zh-CN / zh-TW / en 三语。 */
import { createI18n } from 'vue-i18n'
import en from './locales/en.js'
import zhCN from './locales/zh-CN.js'
import zhTW from './locales/zh-TW.js'

export const SUPPORTED_LOCALES = ['en', 'zh-CN', 'zh-TW']
export const DEFAULT_LOCALE = 'en'

const i18n = createI18n({
  legacy: false,
  globalInjection: true,
  locale: DEFAULT_LOCALE,
  fallbackLocale: DEFAULT_LOCALE,
  messages: { en, 'zh-CN': zhCN, 'zh-TW': zhTW },
})

/* 归一化 locale：不支持时回退默认英语。 */
export function normalizeLocale(code) {
  if (!code) return DEFAULT_LOCALE
  if (SUPPORTED_LOCALES.includes(code)) return code
  // 兼容 zh / zh-Hans / zh-Hant / zh_CN 等写法
  const c = String(code).replace('_', '-')
  if (c.toLowerCase().startsWith('zh')) {
    if (/hant|tw|hk|mo/i.test(c)) return 'zh-TW'
    return 'zh-CN'
  }
  if (c.toLowerCase().startsWith('en')) return 'en'
  return DEFAULT_LOCALE
}

export function setLocale(code) {
  const loc = normalizeLocale(code)
  i18n.global.locale.value = loc
  return loc
}

export function getLocale() {
  return i18n.global.locale.value
}

export default i18n
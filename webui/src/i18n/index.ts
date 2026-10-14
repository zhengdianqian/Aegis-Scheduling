// src/i18n/index.ts
import { createI18n } from 'vue-i18n';
import en from './locales/en';
import zh from './locales/zh';

const messages = {
  en,
  zh
};

// Aegis: 固定中文界面（个人使用，不提供语言切换）
const i18n = createI18n({
  legacy: false,
  locale: 'zh',
  fallbackLocale: 'zh',
  messages,
});

export default i18n;

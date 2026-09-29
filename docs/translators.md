# 译者指南

StockStick 固件内置简体中文，其它语言通过 SD 卡语言包提供，完整机制见 [i18n.md](i18n.md)。

1. 复制 `lib/I18n/translations/english.yaml` 为新文件（或修改已有语言文件）。
2. 设置 `_language_name`（语言本名）、`_language_code`（2–7 位大写代码）、`_order`（不能为 `"0"`，那是内置中文）。
3. 翻译各 `STR_*` 的值；支持 `\\`、`\"`、`\n` 转义；以 `#` 开头的行为注释。
4. 运行 `python3 scripts/build_lang_pack.py <CODE>`，把 `dist/lang/<CODE>.lang` 复制到 SD 卡
   `/.crosspoint/lang/` 后在设备上验证。

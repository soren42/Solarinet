# Vendored fonts

Empty on purpose — font binaries are not checked in here.

Before packaging, drop the three Archivo weights the system uses into this
folder, then run the offline switch:

```
skill/assets/ds/fonts/archivo-400.woff2
skill/assets/ds/fonts/archivo-600.woff2
skill/assets/ds/fonts/archivo-800.woff2

node skill/cli.mjs offline
```

That replaces the Google Fonts `@import` at the top of `styles.css` with local
`@font-face` rules. Until it is run, a composed guide needs network access to
render correctly — and PDF export of a page whose webfont has not loaded falls
back to system-ui, which changes every measurement in the document.

Archivo is SIL OFL 1.1: redistributing the binaries inside the skill package is
permitted. Ship the licence file alongside them.

PHP_ARG_ENABLE(apm, whether to enable apm support,
[  --enable-apm           Enable apm support])

if test "$PHP_APM" != "no"; then
  PHP_NEW_EXTENSION(apm, php_apm.c, $ext_shared)
  PHP_ADD_BUILD_DIR($ext_builddir)
fi

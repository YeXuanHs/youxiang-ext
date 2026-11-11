PHP_ARG_ENABLE([youxiang],
  [whether to enable youxiang support],
  [AS_HELP_STRING([--enable-youxiang],
    [Enable youxiang support (内置授权扩展)])])

if test "$PHP_YOUXIANG" != "no"; then
  PHP_NEW_EXTENSION(youxiang, src/youxiang.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
  PHP_SUBST(YOUXIANG_SHARED_LIBADD)
fi

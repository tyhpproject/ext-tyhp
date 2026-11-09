dnl config.m4 for the tyhp extension

PHP_ARG_ENABLE([tyhp],
  [whether to enable tyhp support],
  [AS_HELP_STRING([--enable-tyhp],
    [Enable tyhp support])],
  [no])

if test "$PHP_TYHP" != "no"; then
  AC_DEFINE([HAVE_TYHP], [1], [Have tyhp support])

  dnl pthread_create is already in libSystem on Darwin. Link -lpthread only
  dnl when the symbol is not already available (typical on Linux).
  TYHP_SHARED_LIBADD=""
  AC_CHECK_FUNC([pthread_create],,
    [PHP_ADD_LIBRARY([pthread], [1], [TYHP_SHARED_LIBADD])])

  dnl AAAA parsing uses dn_expand. macOS names it res_9_dn_expand inside
  dnl libresolv; other platforms keep dn_expand there when it is not in libc.
  AC_CHECK_FUNC([dn_expand],,
    [AC_CHECK_LIB([resolv], [res_9_dn_expand],
      [PHP_ADD_LIBRARY([resolv], [1], [TYHP_SHARED_LIBADD])],
      [AC_CHECK_LIB([resolv], [dn_expand],
        [PHP_ADD_LIBRARY([resolv], [1], [TYHP_SHARED_LIBADD])])])])

  PHP_NEW_EXTENSION([tyhp], [src/tyhp.c src/type_is.c src/type_parse.c src/bind_cache.c src/closure_tree.c src/dns.c], [$ext_shared],, [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1])
  PHP_ADD_BUILD_DIR([$ext_builddir/src])
  PHP_SUBST([TYHP_SHARED_LIBADD])
fi

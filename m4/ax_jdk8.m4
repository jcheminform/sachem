AC_DEFUN([AX_JDK8],
[
    AC_ARG_WITH([jdk8],
        AS_HELP_STRING([--with-jdk8=@<:@ARG@:>@],
            [set the path to your JDK8 directory @<:@default=${JAVA_HOME}@:>@]
        ),
        [
        if test "$withval" == "yes"; then
            JDK8_HOME=${JAVA_HOME}
        elif test "$withval" == "no"; then
            JDK8_HOME=
        else
            JDK8_HOME=${withval}
        fi
        ],
        [
            JDK8_HOME=${JAVA_HOME}
        ]
    )


    AC_MSG_CHECKING([for JDK8])

    if test x"${JDK8_HOME}" != "x"; then
        AC_MSG_RESULT([${JDK8_HOME}])
    else
        AC_MSG_RESULT([no])
        AC_MSG_ERROR([JDK8 was not found])
    fi


    AC_CHECK_FILE([${JDK8_HOME}/jre/lib/amd64/server/libjvm.so],
        [JDK8_LIB=${JDK8_HOME}/jre/lib/amd64/server],
        [AC_CHECK_FILE([${JDK8_HOME}/lib/server/libjvm.so],
            [JDK8_LIB=${JDK8_HOME}/lib/server],
            [AC_MSG_ERROR([JDK8 was not found])])
        ])

    AC_CHECK_FILE([${JDK8_HOME}/include/jni.h],
        [JDK8_INCLUDE=${JDK8_HOME}/include],
        [AC_MSG_ERROR([JDK8 was not found])])


    AC_MSG_CHECKING([whether jni.h supports 1.8])
    
    if grep "^#define JNI_VERSION_1_8 0x00010008$" ${JDK8_INCLUDE}/jni.h > /dev/null; then
        AC_MSG_RESULT([yes])
    else
        AC_MSG_RESULT([no])
        AC_MSG_ERROR([JDK8 was not found])
    fi
    

    JDK8_CPPFLAGS="-I${JDK8_INCLUDE} -I${JDK8_INCLUDE}/linux"
    JDK8_LDFLAGS="-Wl,-rpath,${JDK8_LIB} -L${JDK8_LIB} -ljvm"

    AC_SUBST([JDK8_LDFLAGS])
    AC_SUBST([JDK8_CPPFLAGS])
])

GPHOME=$PWD/trial_python

# Replace with symlink path if it is present and correct
if [ -h ${GPHOME}/../greenplum-db ]; then
    GPHOME_BY_SYMLINK=`(cd ${GPHOME}/../greenplum-db/ && pwd -P)`
    if [ x"${GPHOME_BY_SYMLINK}" = x"${GPHOME}" ]; then
        GPHOME=`(cd ${GPHOME}/../greenplum-db/ && pwd -L)`/.
    fi
    unset GPHOME_BY_SYMLINK
fi
#setup PYTHONHOME
if [ -x $GPHOME/ext/python/bin/python ]; then
    PYTHONHOME="$GPHOME/ext/python"
    export PYTHONHOME
fi
PYTHONPATH=$GPHOME/lib/python
PATH=$GPHOME/bin:$PATH
LD_LIBRARY_PATH=$GPHOME/lib:${LD_LIBRARY_PATH-}
export LD_LIBRARY_PATH
if [ -e $GPHOME/etc/openssl.cnf ]; then
OPENSSL_CONF=$GPHOME/etc/openssl.cnf
export OPENSSL_CONF
fi
export GPHOME
export PATH
export PYTHONPATH

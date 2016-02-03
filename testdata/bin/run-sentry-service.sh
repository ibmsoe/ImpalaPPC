#!/bin/bash
# Copyright (c) 2012 Cloudera, Inc. All rights reserved.

set -euo pipefail
trap 'echo Error in $0 at line $LINENO: $(cd "'$PWD'" && awk "NR == $LINENO" $0)' ERR

. ${IMPALA_HOME}/bin/set-classpath.sh

SENTRY_SERVICE_CONFIG=${SENTRY_CONF_DIR}/sentry-site.xml

# First kill any running instances of the service.
$IMPALA_HOME/testdata/bin/kill-sentry-service.sh

# Sentry picks up JARs from the HADOOP_CLASSPATH and not the CLASSPATH.
export HADOOP_CLASSPATH=${POSTGRES_JDBC_DRIVER}
# Start the service.
${SENTRY_HOME}/bin/sentry --command service -c ${SENTRY_SERVICE_CONFIG} &

# Wait for the service to come online
"$JAVA" -cp $CLASSPATH com.cloudera.impala.testutil.SentryServicePinger \
    --config_file "${SENTRY_SERVICE_CONFIG}" -n 30 -s 2

#!/bin/sh

. ./functions.sh

echo "iscsi-test-cu Prefetch16 test"

start_target
create_disk_lun 1 100M

echo -n "SCSI.Prefetch16 ... "
../test-tool/iscsi-test-cu -i ${IQNINITIATOR} iscsi://${TGTPORTAL}/${IQNTARGET}/1 -t SCSI.Prefetch16 --dataloss > /dev/null || failure
success

shutdown_target
delete_disk_lun 1

exit 0

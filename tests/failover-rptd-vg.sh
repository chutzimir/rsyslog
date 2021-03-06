# This file is part of the rsyslog project, released under GPLv3
echo ===============================================================================
echo \[failover-rptd.sh\]: rptd test for failover functionality
source $srcdir/diag.sh init
source $srcdir/diag.sh startup-vg failover-rptd.conf
source $srcdir/diag.sh injectmsg  0 5000
echo doing shutdown
source $srcdir/diag.sh shutdown-when-empty
echo wait on shutdown
source $srcdir/diag.sh wait-shutdown-vg
source $srcdir/diag.sh check-exit-vg
source $srcdir/diag.sh seq-check  0 4999
source $srcdir/diag.sh exit

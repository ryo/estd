#!/bin/sh

# PROVIDE: estd
# REQUIRE: DAEMON

. /etc/rc.subr

name="estd"
rcvar=$name
command="@PREFIX@/sbin/estd"
pidfile="/var/run/${name}.pid"
command_args="-d"		#always run as a daemon

if [ -f /etc/rc.subr ]; then
	load_rc_config $name
	run_rc_command "$1"
else
	@ECHO@ -n "${name}"
	${command} ${estd_flags} ${command_args}
fi

#!/bin/sh

if test $# -lt 2 -o $# -gt 3; then
    cat >> /dev/stderr <<EOF

Usage:

    $0 <repodir> <summarydir> [ <first-commit> ]

Track <repodir>'s current branch and test each "interesting" commit.
Publish results under <summarydir>.

If <first-commit> is specified, then only go back as far as that
commit when looking for work.  Default is to use <repodir>s current
HEAD as the earliest commit.

XXX: Should this also look at and use things like WEB_PREFIXES and
WEB_WORKERS in Makefile.inc.local?

EOF
    exit 1
fi

set -euvx

webdir=$(dirname $0)
makedir=$(cd ${webdir}/../.. && pwd)
utilsdir=${makedir}/testing/utils
repodir=$(cd $1 && pwd ) ; shift
summarydir=$(cd $1 && pwd) ; shift
if test $# -gt 0 ; then
    first_commit=$1 ; shift
else
    first_commit=$(cd ${repodir} && git show --no-patch --format=%H HEAD)
fi

status() {
    ${webdir}/json-status.sh --json ${summarydir}/status.json "$@"
    cat <<EOF

--------------------------------------

    $*

--------------------------------------

EOF
}

run() {
    ${status} "running 'make $1'"

    # fudge up enough of summary.json to fool the top level
    if test ! -r ${resultsdir}/kvm-test.ok ; then
	${webdir}/json-summary.sh "${start_time}" > ${resultsdir}/summary.json
    fi

    # XXX: So new features can be tested, prefer the local version of
    # runner in ${utilsdir}.  Once things are in sync this can go
    # away.
    #
    # The catch is that the sanitizer files in ${repodir} may not know
    # how to sanitize out >>cut>> lines generated by the local
    # kvmrunner.

    if grep '>>cut>>' ${repodir}/testing/sanitizers/cutout.sed > /dev/null ; then
	runner="${utilsdir}/kvmrunner.py --publish-results ${resultsdir} --testing-directory ${repodir}/testing --publish-status ${summarydir}/status.json"
    else
	runner=
    fi

    # XXX: disable publishing when running make in the ${repodir}.  It
    # will likely conflict with the above kvmrunner command.

    if make -C ${repodir} $1 \
	    WEB_REPODIR= \
	    WEB_RESULTSDIR= \
	    WEB_SUMMARYDIR= \
	    ${runner:+KVMRUNNER="${runner}"} \
	    ${prefixes:+KVM_PREFIXES="${prefixes}"} \
	    ${workers:+KVM_WORKERS="${workers}"} \
	    2>&1 ; then
	touch ${resultsdir}/$1.ok ;
    fi | if test -r ${webdir}/$1-status.awk ; then
	awk -v script="${status}" -f ${webdir}/$1-status.awk
    else
	cat
     fi | tee -a ${resultsdir}/$1.log
    if test ! -r ${resultsdir}/$1.ok ; then
	${status} "'make $1' failed"
	exit 1
    fi
}

while true ; do

    # Time has passed (or the script was restarted), download any more
    # recent commits, and pull all the updates into ${branch}.  Force
    # ${branch} to be identical to ${remote} by using --ff-only - if
    # it fails the script dies.

    status "updating repo"
    ( cd ${repodir} && git fetch || true )
    ( cd ${repodir} && git merge --ff-only )

    # if results with output-missing start to show up, that is a good
    # sign that the VMs have become corrupted and need a rebuild.

    status "checking KVMs"
    if grep '"output-missing"' "${summarydir}"/*-g*/results.json > /dev/null ; then
	status "corrupt domains detected, deleting old"
	( cd ${repodir} && make kvm-purge )
	status "corrupt domains detected, deleting bogus results"
	grep '"output-missing"' "${summarydir}"/*-g*/results.json \
	    | sed -e 's;/results.json.*;;' \
	    | sort -u \
	    | xargs --max-args=1 --verbose --no-run-if-empty rm -rf
	status "corrupt domains detected, building fresh domains"
	( cd ${repodir} && make kvm-install-test-domains )
    fi

    # update the summary, if necessary add more commits using
    # ${repodir}

    status "updating summary"
    make -C ${makedir} web-summarydir \
	 WEB_REPODIR=${repodir} \
	 WEB_RESULTSDIR= \
	 WEB_SUMMARYDIR=${summarydir}

    # Starting with HEAD, work backwards looking for anything
    # untested.

    status "looking for work"
    if ! hash=$(${webdir}/gime-work.sh ${summarydir} ${repodir} ${first_commit}) ; then \
	# Seemlingly nothing to do.
	seconds=$(expr 60 \* 60 \* 3)
	now=$(date +%s)
	future=$(expr ${now} + ${seconds})
	status "idle; will retry $(date -u -d @${future} +%H:%M)"
	sleep ${seconds}
	continue
    fi

    # Now discard everything back to the commit to be tested, making
    # that HEAD.  This could have side effects such as switching
    # branches, take care.

    status "checking out ${hash}"
    ( cd ${repodir} && git reset --hard ${hash} )

    # Use web-targets.mk to compute RESULTSDIR so things are
    # consistent.

    resultsdir=${summarydir}/$(${webdir}/gime-git-description.sh ${repodir})
    gitstamp=$(basename ${resultsdir})
    status="${webdir}/json-status.sh \
      --json ${summarydir}/status.json \
      --directory ${gitstamp}"

    # Test what has been made HEAD.
    #
    # Only run the testsuite and update the web site when the current
    # commit looks interesting.  The heuristic is trying to identify
    # coding and testsuite changes; while ignoring infrastructure.

    start_time=$(${webdir}/now.sh)

    # create the resultsdir and point the summary at it.
    ${status} "creating results directory"
    make -C ${makedir} web-resultsdir \
	 WEB_TIME=${start_time} \
	 WEB_REPODIR=${repodir} \
	 WEB_RESULTSDIR=${resultsdir} \
	 WEB_SUMMARYDIR=${summarydir}

    run kvm-shutdown
    run distclean
    run kvm-install
    run kvm-keys
    run kvm-test

    ${status} "build succeeded"
    make -C ${makedir} \
	 web-summarydir \
	 WEB_REPODIR=${repodir} \
	 WEB_RESULTSDIR= \
	 WEB_SUMMARYDIR=${summarydir}

done

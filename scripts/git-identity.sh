#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Shared Git identity contract for repository tooling.
#
# Tooling that replays, rebases or audits a checkout creates commits, but it
# never carries an identity of its own: it commits as whoever runs it. Source
# this file and call uml_require_git_identity before any such step.
#
# The identity is resolved once from the ambient environment, so
# GIT_AUTHOR_*/GIT_COMMITTER_* variables and the caller's configured
# user.name/user.email both work. user.useConfigOnly stops Git from inventing
# an identity from the account and host names, which would otherwise silently
# stamp a machine-local identity onto replayed commits. The resolved values
# are exported so the helpers that run Git with GIT_CONFIG_NOSYSTEM=1 and
# GIT_CONFIG_GLOBAL=/dev/null still see them.

UML_GIT_IDENTITY_ADVICE='Git identity is not configured: set user.name and user.email (git config --global) or GIT_COMMITTER_NAME/GIT_COMMITTER_EMAIL'

uml_git_identity_field()
{
	uml_git_identity_variable=$1
	uml_git_identity_value=$(git -c user.useConfigOnly=true var \
		"$uml_git_identity_variable" 2>/dev/null) || uml_git_identity_value=
	case $uml_git_identity_value in
	*' <'*'> '*) ;;
	*) return 1 ;;
	esac
	uml_git_identity_name=${uml_git_identity_value%% <*}
	uml_git_identity_rest=${uml_git_identity_value#*<}
	uml_git_identity_email=${uml_git_identity_rest%%>*}
	[ -n "$uml_git_identity_name" ] && [ -n "$uml_git_identity_email" ]
}

# uml_require_git_identity [CONTEXT]
#
# Exit with the shared advice when no identity is available. CONTEXT is the
# caller's usual message prefix; it defaults to the running script.
uml_require_git_identity()
{
	uml_git_identity_context=${1:-${0##*/}}
	if ! uml_git_identity_field GIT_COMMITTER_IDENT; then
		printf '%s: %s\n' "$uml_git_identity_context" \
			"$UML_GIT_IDENTITY_ADVICE" >&2
		exit 1
	fi
	GIT_COMMITTER_NAME=$uml_git_identity_name
	GIT_COMMITTER_EMAIL=$uml_git_identity_email
	# Replayed commits keep the author recorded in their mail patch. An
	# author is still needed for a commit created from scratch, and a
	# caller who named only a committer has named who is running the tool.
	if ! uml_git_identity_field GIT_AUTHOR_IDENT; then
		uml_git_identity_name=$GIT_COMMITTER_NAME
		uml_git_identity_email=$GIT_COMMITTER_EMAIL
	fi
	GIT_AUTHOR_NAME=$uml_git_identity_name
	GIT_AUTHOR_EMAIL=$uml_git_identity_email
	export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL
	export GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL
	unset uml_git_identity_variable uml_git_identity_value
	unset uml_git_identity_name uml_git_identity_rest uml_git_identity_email
}

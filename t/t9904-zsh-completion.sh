#!/bin/sh

test_description='test Zsh completion'

. ./test-lib.sh

if ! test_have_prereq ZSH
then
	skip_all='skipping Zsh completion tests, zsh not available'
	test_done
fi

run_completion()
{
	zsh -c '
	# Load the pseudo terminal module.
	zmodload zsh/zpty

	# initialize the pty:
	# - start zsh, load completion, and configure completion for Git
	# - take care not to invoke _git
	zpty comptest zsh -ife
	zpty -w comptest <<-\EOF
		autoload -U compinit && compinit
		zstyle ":completion:*:*:git:*" script $GIT_BUILD_DIR/contrib/completion/git-completion.bash
		. =($GIT_BUILD_DIR/contrib/completion/git-completion.zsh | sed /^_git\$/d)
		compdef _git git

	# trigger completion
	…
	# trigger test case
	…
EOF
	' zsh "$@"
}

test_expect_success 'completion does not leak local __git_repo_path' '
	test_create_repo tmp &&
	(
		cd tmp &&
		git commit -m init --allow-empty &&
		git branch bar
		run_completion "git rebase" "! declare -p __git_repo_path"
	)
'

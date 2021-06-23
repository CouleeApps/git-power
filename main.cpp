#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>
#include <git2.h>
// Did I use c++ only for its threads? Maybe
#include <thread>

struct result_t {
	git_time author_time;
	git_time committer_time;
};

std::mutex m;
std::condition_variable cv;
std::atomic<result_t> result;
std::atomic<bool> finished;
std::atomic<int> attempts;

void try_commits(size_t i, size_t threads, size_t bits, git_repository *repository,
                 git_signature *author_, git_signature *committer_,
                 const char *message_encoding, const char *message, git_tree *tree,
                 size_t parent_count, const git_commit **parents) {
	// Copy these so we don't clobber the other threads' resources
	git_signature *author;
	git_signature_dup(&author, author_);

	git_signature *committer;
	git_signature_dup(&committer, committer_);

	// Instead of making new commits every time, we can just edit metadata on the existing
	// commit and see what hashes we get. Easiest metadata to modify is the timestamp, so
	// just add 1 to every field until we get a good hash. So create one commit and just
	// start modifying it.
	git_buf buf{};
	git_commit_create_buffer(&buf, repository, author, committer, message_encoding,
	                         message, tree, parent_count, parents);

	/*
tree d11fc77c07df4faadf669a4e397714a1bd588f5d
parent 0000255c99724e379f925df516265e9535e3feb0
author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
committer Glenn Smith <couleeapps@gmail.com> 1624473924 -0302

Test

	 */

	// Find point in buf where we change timestamps
	char *author_ts = nullptr;
	char *committer_ts = nullptr;
	size_t author_ts_len = 0;
	size_t committer_ts_len = 0;

	// Evil pointer shit
	for (char *start = buf.ptr; start < buf.ptr + buf.size; start++) {
		if (strncmp(start, "\nauthor ", strlen("\nauthor ")) == 0 ||
		    strncmp(start, "\ncommitter ", strlen("\ncommitter ")) == 0) {
			// Author/Committer line

			char *line_end = start;
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//^
			size_t len = 0;
			do {
				line_end++;
			} while (*line_end != '\n');
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                                          ^
			line_end--;
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                                         ^
			do {
				line_end--;
				len++;
			} while (isalnum(*line_end));
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                                     ^----
			line_end--;
			len++;
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                                    ^-----
			line_end--;
			len++;
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                                   ^------
			do {
				line_end--;
				len++;
			} while (isalnum(*line_end));
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                         ^---------------
			line_end++;
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                          ^---------------
			if (strncmp(start, "\nauthor ", strlen("\nauthor ")) == 0) {
				author_ts = line_end;
				author_ts_len = len;
			} else {
				committer_ts = line_end;
				committer_ts_len = len;
			}
		}
	}

	// Sanity
	assert(author_ts_len >= 16);
	assert(committer_ts_len >= 16);

	// For simplicity, make these always positive
	author->when.offset = 0;
	author->when.sign = '+';
	committer->when.offset = (int)i;
	committer->when.sign = '+';

	int test;
	do {
		test = 0;
		// See if another thread got it
		if (finished)
			return;

		// Increment timezone by 1
		author->when.offset++;
		// Because timezones are wack, don't bother going past +1200
		if (author->when.offset > 12 * 60) {
			author->when.offset = 0;
			committer->when.offset += (int)threads;
		}
		// +1200 on both --> increase timestamp by 1 and try again
		if (committer->when.offset > 12 * 60) {
			committer->when.offset = (int)i;
			author->when.time += 1;
		}

		// Using sprintf is probably not the fastest (30% of runtime)
		size_t nul = sprintf(author_ts, "%010lld +%02d%02d", author->when.time,
		                     author->when.offset / 60, author->when.offset % 60);
		// Also it null-terminates
		author_ts[nul] = '\n';
		assert(nul == author_ts_len);
		nul = sprintf(committer_ts, "%010lld +%02d%02d", committer->when.time,
		              committer->when.offset / 60, committer->when.offset % 60);
		committer_ts[nul] = '\n';
		assert(nul == committer_ts_len);

		// The slow part (70% of runtime)
		git_oid hash;
		git_odb_hash(&hash, buf.ptr, buf.size, GIT_OBJECT_COMMIT);

		// Make sure it matches
		for (size_t n = 0; n < bits; n++) {
			test |= (hash.id[n / 8] >> (7 - (n % 8))) & 1;
		}
		attempts++;
	} while (test != 0);

	// Big sanity check here since we think this is a good hash

	git_oid hash;
	git_buf buf2;
	git_commit_create_buffer(&buf2, repository, author, committer, message_encoding,
	                         message, tree, parent_count, parents);
	git_odb_hash(&hash, buf2.ptr, buf2.size, GIT_OBJECT_COMMIT);
	for (size_t n = 0; n < bits; n++) {
		test |= (hash.id[n / 8] >> (7 - (n % 8))) & 1;
	}

	// Hopefully these never fail
	assert(strcmp(buf.ptr, buf2.ptr) == 0);
	assert(test == 0);

	git_buf_dispose(&buf2);
	git_buf_dispose(&buf);

	// Report results
	result = result_t{
		.author_time = author->when,
		.committer_time = committer->when,
	};
	finished = true;
	cv.notify_all();
}

int main(int argc, const char **argv) {
	// Arg parsing could be improved
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <bits> <threads>\n", argv[0]);
		return 1;
	}
	int bits;
	int threads;
	sscanf("%d", argv[1], &bits);
	sscanf("%d", argv[2], &threads);

	git_libgit2_init();

	char dir[0x400];
	getcwd(dir, 0x400);

	// Find our repo
	git_repository *repository;
	git_repository_open(&repository, dir);

	git_reference *head;
	git_repository_head(&head, repository);

	const git_oid *target = git_reference_target(head);

	// Copy from the latest commit
	git_commit *commit;
	git_commit_lookup(&commit, repository, target);

	git_tree *tree;
	git_commit_tree(&tree, commit);

	size_t parent_count = git_commit_parentcount(commit);

	auto **parents = new git_commit *[parent_count];
	for (size_t i = 0; i < parent_count; i++) {
		git_commit_parent(&parents[i], commit, i);
	}

	git_signature *author;
	git_signature_dup(&author, git_commit_author(commit));

	git_signature *committer;
	git_signature_dup(&committer, git_commit_committer(commit));

	const char *message_encoding = git_commit_message_encoding(commit);
	const char *message = git_commit_message(commit);

	// Setup parallelization
	finished = false;
	attempts = 0;
	timeval start{};
	gettimeofday(&start, nullptr);
	git_oid hash;

	std::unique_lock<std::mutex> lk(m);

	// Start!!!
	for (size_t i = 0; i < threads; i++) {
		std::thread([=]() {
			try_commits(i, threads, bits, repository, author, committer, message_encoding,
			            message, tree, parent_count, (const git_commit **)parents);
		}).detach();
	}

	// A thread will signal this when it finds a match
	cv.wait(lk);

	// Extract results
	result_t r = result;
	int a = attempts;
	author->when = r.author_time;
	committer->when = r.committer_time;

	// And make the commit for them
	git_commit_create(&hash, repository, nullptr, author, committer,
	                  message_encoding,
	                  message, tree, parent_count,
	                  (const git_commit **)parents);

	// Fancy print
	char id[0x100];
	git_oid_tostr(id, 0x100, &hash);

	timeval end{};
	gettimeofday(&end, nullptr);

	timeval difference{};
	timersub(&end, &start, &difference);

	printf("Found commit hash %s in %d attempts in %ld.%06d time\n", id, a,
	       difference.tv_sec, difference.tv_usec);

	// Soft reset to this commit so it is now branch head
	git_object *new_commit;
	git_object_lookup(&new_commit, repository, &hash, GIT_OBJECT_COMMIT);

	git_reset(repository, new_commit, GIT_RESET_SOFT, nullptr);

	// Cleanup
	delete[] parents;
	git_tree_free(tree);
	git_commit_free(commit);

	git_reference_free(head);
	git_repository_free(repository);

	git_libgit2_shutdown();

	return 0;
}

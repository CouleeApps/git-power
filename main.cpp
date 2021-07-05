#include <assert.h>
#include <ctype.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
// Why tho
#define NOMINMAX
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

// Did I use c++ only for its threads? Maybe
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <git2.h>
#include <openssl/crypto.h>
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>

struct result_t {
	char buf[0x1000];
	size_t buf_size;
};

std::mutex m;
std::condition_variable cv;
std::atomic<result_t> result;
std::atomic<bool> finished;
std::atomic<bool> success;
std::atomic<int> closest_bits;
std::atomic<git_oid> closest;
std::atomic<uint64_t> attempts;

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

	// Add a line for ourselves
	char *modified_commit = new char[buf.size + 0x100];
	size_t commit_size = 0;

	// "nonce <num>\n<commit>"
	commit_size += snprintf(modified_commit + commit_size, 0x100, "nonce 000000000001\n");
	memcpy(modified_commit + commit_size, buf.ptr, buf.size);
	commit_size += buf.size;
	modified_commit[commit_size] = 0;

	// When git sha1's the commit, it includes the metadata for object type and size
	// In practice this just adds "commit <length>\0" to the front of the buffer being
	// passed into SHA1. So by precomputing this ourselves we can save all of the error
	// checking done by git, and use our own SHA1 implementation (which is likely faster).

	char *obj_buf = new char[buf.size + 0x100];
	size_t obj_size = 0;
	size_t header_size = 0;

	// "commit <length>\0"
	header_size += snprintf(obj_buf, 0x100, "commit %zd", buf.size);
	obj_buf[header_size] = 0;
	header_size += 1;

	char *commit_buf = obj_buf + header_size;
	memcpy(commit_buf, modified_commit, commit_size);
	commit_buf[commit_size] = 0;

	obj_size += header_size;
	obj_size += commit_size;

	// Null terminate for sanity
	obj_buf[obj_size] = 0;

	/* Now we have:

commit 274\0tree d11fc77c07df4faadf669a4e397714a1bd588f5d
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
	for (char *start = obj_buf; start < obj_buf + obj_size; start++) {
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
			//                                                     ^---
			line_end--;
			len++;
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                                    ^----
			line_end--;
			len++;
			//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
			//                                                   ^-----
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

	// Normalize
	//author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
	//                                                     ^
	author_ts[author_ts_len - 5] = '+';
	author_ts[author_ts_len - 4] = '0';
	author_ts[author_ts_len - 3] = '0';
	author_ts[author_ts_len - 2] = '0';
	author_ts[author_ts_len - 1] = '0';
	committer_ts[committer_ts_len - 5] = '+';
	committer_ts[committer_ts_len - 4] = '0';
	committer_ts[committer_ts_len - 3] = '0';
	committer_ts[committer_ts_len - 2] = '0';
	committer_ts[committer_ts_len - 1] = '0';

	// For simplicity, make these always positive
	author->when.offset = 0;
	author->when.sign = '+';
	committer->when.offset = (int)i;
	committer->when.sign = '+';

	int good_bits;
	int local_closest = 0;
	do {
		// See if another thread got it
		if (finished)
			return;

		// Increment timezone by 1
		author->when.offset++;

		// Because timezones are wack, don't bother going past +1200
		if (author->when.offset > 12 * 60) {
			author->when.offset = 0;
			committer->when.offset += (int)threads;
			// Extract the number of the timezone offset by individual chars
			committer_ts[committer_ts_len - 4] = '0' + (committer->when.offset / 600 % 10);
			committer_ts[committer_ts_len - 3] = '0' + (committer->when.offset / 60 % 10);
			committer_ts[committer_ts_len - 2] = '0' + (committer->when.offset / 10 % 6);
			committer_ts[committer_ts_len - 1] = '0' + (committer->when.offset % 10);
		}
		// After the above in case of wrap
		author_ts[author_ts_len - 4] = '0' + (author->when.offset / 600 % 10);
		author_ts[author_ts_len - 3] = '0' + (author->when.offset / 60 % 10);
		author_ts[author_ts_len - 2] = '0' + (author->when.offset / 10 % 6);
		author_ts[author_ts_len - 1] = '0' + (author->when.offset % 10);

		// +1200 on both --> increase timestamp by 1 and try again
		if (committer->when.offset > 12 * 60) {
			committer->when.offset = (int)i;
			author->when.time += 1;

			// Probably a faster way to do this
			size_t nul = sprintf(author_ts, "%010lld +%02d%02d", author->when.time,
			                     author->when.offset / 60, author->when.offset % 60);
			// Also it null-terminates
			author_ts[nul] = '\n';
			assert(nul == author_ts_len);
			nul = sprintf(committer_ts, "%010lld +%02d%02d", committer->when.time,
			              committer->when.offset / 60, committer->when.offset % 60);
			committer_ts[nul] = '\n';
			assert(nul == committer_ts_len);
		}

		// The slow part
		git_oid hash;
		unsigned int md_len;
		// Or insert your favorite SHA1 function here
		EVP_Digest(obj_buf, obj_size, &hash.id[0], &md_len, EVP_sha1(), NULL);

		// If it's not 20 then we just demolished the stack lmao
		assert(md_len == 20);

		// Make sure it matches
		good_bits = 0;
		int still_good = 1;
		for (int n = 0; n < bits; n++) {
			int bit = ((hash.id[n / 8] >> (7 - (n % 8))) & 1) ^ 1;
			still_good &= bit;
			good_bits += still_good;
		}
		if (good_bits > local_closest) {
			local_closest = good_bits;
			if (good_bits > closest_bits) {
				closest_bits = good_bits;
				closest = hash;
			}
		}
		attempts++;
	} while (good_bits != bits);

	// Big sanity check here since we think this is a good hash
	git_oid hash;
	git_odb_hash(&hash, commit_buf, commit_size, GIT_OBJECT_COMMIT);
	int test = 0;
	for (size_t n = 0; n < bits; n++) {
		test |= (hash.id[n / 8] >> (7 - (n % 8))) & 1;
	}

	// Hopefully this never fails
	assert(test == 0);

	{
		std::unique_lock<std::mutex> l(m);

		if (!finished) {
			// Report results
			result_t local_result{};
			local_result.buf_size = commit_size;
			memcpy(local_result.buf, commit_buf, commit_size);

			result = local_result;
			success = true;
			finished = true;
			cv.notify_all();
		}
	}

	git_buf_dispose(&buf);
	delete [] obj_buf;
}

void sigint_handler(int sig) {
	success = false;
	finished = true;
	cv.notify_all();
}

int main(int argc, const char **argv) {
	// Arg parsing could be improved
	if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)) {
		fprintf(stderr, "Usage: %s [bits [threads]]\n", argv[0]);
		return 1;
	}
	// Sensible defaults
	int bits = 32;
	int threads = std::max((int)std::thread::hardware_concurrency(), 1);
	if (argc > 1)
		sscanf(argv[1], "%d", &bits);
	if (argc > 2)
		sscanf(argv[2], "%d", &threads);

	git_libgit2_init();
	ERR_load_CRYPTO_strings();
	OpenSSL_add_all_algorithms();
	OPENSSL_no_config();

	char dir[0x400];
	getcwd(dir, 0x400);

	// Find our repo
	git_repository *repository;
	git_repository_open(&repository, dir);

	if (repository == nullptr) {
		fprintf(stderr, "Current directory is not a git repository.\n");
		git_libgit2_shutdown();
		return 2;
	}

	git_reference *head;
	git_repository_head(&head, repository);

	if (head == nullptr) {
		fprintf(stderr, "Current repository does not have a HEAD.\n");
		git_repository_free(repository);
		git_libgit2_shutdown();
		return 3;
	}

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
	success = false;
	finished = false;
	attempts = 0;
	auto start = std::chrono::high_resolution_clock::now();

	std::unique_lock<std::mutex> lk(m);
	signal(SIGINT, &sigint_handler);

	std::vector<std::thread> spawned_threads;

	// Start!!!
	for (size_t i = 0; i < threads; i++) {
		spawned_threads.emplace_back([=]() {
			try_commits(i, threads, bits, repository, author, committer, message_encoding,
			            message, tree, parent_count, (const git_commit **)parents);
		});
	}

	// A thread will signal this when it finds a match
	while (!finished) {
		cv.wait_for(lk, std::chrono::seconds(1));

		git_oid close = closest;
		uint64_t a = attempts;
		int b = closest_bits;

		char id[0x100];
		git_oid_tostr(id, 0x100, &close);

		auto end = std::chrono::high_resolution_clock::now();
		auto difference = end - start;

		fprintf(stderr, "\rRuns: %12llu Best found: %s (%d/%d bits) Time: %lld.%06lld ~%fMH/s", a, id,
		        b, bits, std::chrono::duration_cast<std::chrono::seconds>(difference).count(),
		        std::chrono::duration_cast<std::chrono::microseconds>(difference).count() % 1000000,
		        (double)a / (double)std::chrono::duration_cast<std::chrono::microseconds>(difference).count());
	}

	// Wait for threads
	for (auto& thread: spawned_threads) {
		thread.join();
	}

	// Extract results
	if (success) {
		result_t r = result;
		char commit_buffer[0x1000];
		size_t commit_size = r.buf_size;
		memcpy(commit_buffer, r.buf, r.buf_size);

		git_odb *db;
		git_repository_odb(&db, repository);

		// And make the commit for them
		git_oid hash;
		git_odb_write(&hash, db, commit_buffer, commit_size, GIT_OBJECT_COMMIT);

		// Fancy print
		char id[0x100];
		git_oid_tostr(id, 0x100, &hash);

		printf("\nFound commit hash %s\n", id);

		// Soft reset to this commit so it is now branch head
		git_object *new_commit;
		git_object_lookup(&new_commit, repository, &hash, GIT_OBJECT_COMMIT);

		git_reset(repository, new_commit, GIT_RESET_SOFT, nullptr);
	} else {
		printf("\nUser cancelled\n");
	}

	uint64_t a = attempts;

	auto end = std::chrono::high_resolution_clock::now();
	auto difference = end - start;
	printf("Stats: %llu attempts in %lld.%06lld time\n", a,
	       std::chrono::duration_cast<std::chrono::seconds>(difference).count(),
	       std::chrono::duration_cast<std::chrono::microseconds>(difference).count() % 1000000);

	// Cleanup
	delete[] parents;
	git_tree_free(tree);
	git_commit_free(commit);

	git_reference_free(head);
	git_repository_free(repository);

	git_libgit2_shutdown();

	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
	ERR_free_strings();

	return 0;
}

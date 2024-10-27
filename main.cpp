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
	// Entire commit buffer (not including "commit 123" header)
	char buf[0x1000];
	size_t buf_size;
};

std::mutex m;
std::condition_variable cv;
// Can you have an atomic of length 0x1008? There's no way that's fast.
std::atomic<result_t> result;
std::atomic<bool> finished;
std::atomic<bool> success;
std::atomic<int> closest_bits;
std::atomic<git_oid> closest;
std::atomic<uint64_t> attempts;

void try_commits(size_t i, size_t threads, size_t bits, char *base_body, size_t base_size) {
	// We need to modify the original commit data to add a nonce for brute forcing
	// But where do we do that?
	// - If the commit is not GPG signed, we can just add an extra field into the commit
	//   details with the nonce. One pitfall is that git expects the `tree` field to be
	//   first, so we have to do it at the end.
	// - If the commit is GPG signed, we can't edit anything outside the signature wihout
	//   breaking it. So...... just add it INSIDE the GPG signature!!
	//   GPG doesn't check header values inside the signature, so we can just inject a new
	//   header with the nonce and brute force that. Any GPG client will just ignore the
	//   unknown header and do the signature check on the rest of the commit (which we
	//   have not touched). This is probably the most cursed part of this whole project.
	// So let's do that.

	/*
UNSIGNED

tree d11fc77c07df4faadf669a4e397714a1bd588f5d
parent 0000255c99724e379f925df516265e9535e3feb0
author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
committer Glenn Smith <couleeapps@gmail.com> 1624473924 -0302

Test

OR SIGNED

tree 31c2eae32101258cfffc2e85f1d2d790b229a9ad
parent 8dfe0ce00895c6a8f55bbd30d2543c1945fcad15
author Glenn Smith <couleeapps@gmail.com> 1625532218 -0400
committer Glenn Smith <couleeapps@gmail.com> 1625532226 -0400
gpgsig -----BEGIN PGP SIGNATURE-----
 Comment: Created with Krypton

 iF4EABMKAAYFAmDjp0IACgkQm3HsKD8LehTDDwD/c9DB6IxtAXzF55FjgqevfoNO
 Eegmn53HSsYrRHOE9ZMA/Av/moBkr9Qh/nTO2c3XVfa228grAOIwcSYkn9s6WS25
 =XM24
 -----END PGP SIGNATURE-----

I should not be allowed near this

 */

	// Probably large enough for our shenanigans
	char *modified_commit = new char[base_size + 0x100];
	size_t commit_size = 0;

	// Figure out where to insert our nonce header/field
	char *nonce_insert;
	// Offset into the commit data where our nonce starts
	size_t nonce_index;

	// Mega cursed: Check if this is gpg-signed
	char *gpgsig = strstr(base_body, "-----BEGIN PGP SIGNATURE-----");
	if (gpgsig == nullptr) {
		// No it's not: insert nonce as a field right before the commit message
		nonce_insert = strstr(base_body, "\n\n");

		// Copy commit until the nonce shenanigans
		// No, this pointer math is not safe AT ALL
		memcpy(modified_commit, base_body, nonce_insert - base_body);
		commit_size += nonce_insert - base_body;

		// Have we modified this commit before? Git will probably not be happy if we have
		// two copies of the same field
		char *previous_nonce = strstr(base_body, "\nnonce ");
		if (previous_nonce == nullptr) {
			// Insert nonce header/field
			// NB: Start with \n because we're going right before the newline(s) in the
			//     header, and don't add one at the end for the same reason.

			// Nonce field in commit fields: "\nnonce 01AAAAAAAAAAAAAAAA"
			//                                        ^
			nonce_index = commit_size + 7;
			commit_size += snprintf(modified_commit + commit_size, 0x100,
			                        "\nnonce %zu%zuAAAAAAAAAAAAAAAA", i, threads);
		} else {
			nonce_index = (previous_nonce - base_body) + 9;
		}
	} else {
		// Yes it is: insert nonce as a header in the GPG signature
		nonce_insert = strstr(gpgsig, "\n\n");

		memcpy(modified_commit, base_body, nonce_insert - base_body);
		commit_size += nonce_insert - base_body;

		// Have we modified this commit before? GPG will probably not be happy if we have
		// two copies of the same field
		char *previous_nonce = strstr(base_body, "\n Nonce: ");
		if (previous_nonce == nullptr) {
			// Nonce header in GPG signature: "\n Nonce: 01AAAAAAAAAAAAAAAA"
			//                                           ^
			// Important: There is a space at the start of every line in the signature
			nonce_index = commit_size + 9;
			commit_size += snprintf(modified_commit + commit_size, 0x100,
			                        "\n Nonce: %zu%zuAAAAAAAAAAAAAAAA", i, threads);
		} else {
			nonce_index = (previous_nonce - base_body) + 11;
		}
	}
	// Copy the rest of the commit after
	memcpy(modified_commit + commit_size, nonce_insert, base_size - (nonce_insert - base_body));
	commit_size += base_size - (nonce_insert - base_body);
	// Null terminate because I'm paranoid
	modified_commit[commit_size] = 0;

	// When git sha1's the commit, it includes the metadata for object type and size
	// In practice this just adds "commit <length>\0" to the front of the buffer being
	// passed into SHA1. So by precomputing this ourselves we can save all of the error
	// checking done by git, and we can use our own SHA1 implementation (which is faster).

	char *obj_buf = new char[base_size + 0x100];
	size_t obj_size = 0;
	size_t header_size = 0;

	// "commit <length>\0"
	header_size += snprintf(obj_buf, 0x100, "commit %zd", commit_size);
	obj_buf[header_size] = 0;
	header_size += 1;

	// Then append our hacked-up commit body
	char *commit_buf = obj_buf + header_size;
	memcpy(commit_buf, modified_commit, commit_size);
	commit_buf[commit_size] = 0;

	// Not the best way to calculate size
	obj_size += header_size;
	obj_size += commit_size;

	// Null terminate for sanity
	obj_buf[obj_size] = 0;

	delete [] modified_commit;

	// Offset into the real buffer by the offset in the hacked-up buffer
	char *nonce_start = commit_buf + nonce_index;

	/* Now we have:

commit 246\0
tree d11fc77c07df4faadf669a4e397714a1bd588f5d
parent 0000255c99724e379f925df516265e9535e3feb0
author Glenn Smith <couleeapps@gmail.com> 1624473924 +0613
committer Glenn Smith <couleeapps@gmail.com> 1624473924 -0302
nonce 01AAAAAAAAAAAAAAAA

Test

OR
commit 516\0
tree 31c2eae32101258cfffc2e85f1d2d790b229a9ad
parent 8dfe0ce00895c6a8f55bbd30d2543c1945fcad15
author Glenn Smith <couleeapps@gmail.com> 1625532218 -0400
committer Glenn Smith <couleeapps@gmail.com> 1625532226 -0400
gpgsig -----BEGIN PGP SIGNATURE-----
 Comment: Created with Krypton

 iF4EABMKAAYFAmDjp0IACgkQm3HsKD8LehTDDwD/c9DB6IxtAXzF55FjgqevfoNO
 Eegmn53HSsYrRHOE9ZMA/Av/moBkr9Qh/nTO2c3XVfa228grAOIwcSYkn9s6WS25
 =XM24
 -----END PGP SIGNATURE-----
 Nonce: 01LOIIFMAAAAAAAAAA

I should not be allowed near this

	 */
	EVP_MD_CTX *md_ctx_shared = EVP_MD_CTX_new();
	EVP_DigestInit_ex(md_ctx_shared, EVP_sha1(), nullptr);
	EVP_DigestUpdate(md_ctx_shared, obj_buf, nonce_start - obj_buf);
	EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(md_ctx, EVP_sha1(), nullptr);

	// Local copies of atomics because gotta go fast
	uint8_t good_bits;
	uint8_t local_closest = 0;

	// I hope 64 bits is enough
	uint64_t nonce = 0;

	do {
		// See if another thread got it
		if (finished) {
			EVP_MD_CTX_free(md_ctx);
			EVP_MD_CTX_free(md_ctx_shared);
			delete [] obj_buf;
			return;
		}

		// Increment nonce by 1
		nonce_start[0] = ' ' + (nonce & 0x3F);
		// Micro optimizing: these two first branches are slower than always writing
//		if ((nonce & 0x3f) == 0)
			nonce_start[1] = ' ' + ((nonce >> 6) & 0x3F);
//		if ((nonce & 0xfff) == 0)
			nonce_start[2] = ' ' + ((nonce >> 12) & 0x3F);
		if ((nonce & 0x3ffff) == 0)
			nonce_start[3] = ' ' + ((nonce >> 18) & 0x3F);
		if ((nonce & 0xffffff) == 0)
			nonce_start[4] = ' ' + ((nonce >> 24) & 0x3F);
		if ((nonce & 0x3fffffff) == 0)
			nonce_start[5] = ' ' + ((nonce >> 30) & 0x3F);
		if ((nonce & 0xfffffffff) == 0)
			nonce_start[6] = ' ' + ((nonce >> 36) & 0x3F);
		if ((nonce & 0x3ffffffffff) == 0)
			nonce_start[7] = ' ' + ((nonce >> 42) & 0x3F);
		if ((nonce & 0xffffffffffff) == 0)
			nonce_start[8] = ' ' + ((nonce >> 48) & 0x3F);
		if ((nonce & 0x3fffffffffffff) == 0)
			nonce_start[9] = ' ' + ((nonce >> 54) & 0x3F);
		if ((nonce & 0xfffffffffffffff) == 0)
			nonce_start[10] = ' ' + ((nonce >> 60) & 0x3F);

		// The slow part
		git_oid hash;
		unsigned int md_len;
		// Or insert your favorite SHA1 function here
		EVP_MD_CTX_copy_ex(md_ctx, md_ctx_shared);
		EVP_DigestUpdate(md_ctx, nonce_start, obj_size - (nonce_start - obj_buf));
		EVP_DigestFinal_ex(md_ctx, &hash.id[0], &md_len);

		// If it's not 20 then we just demolished the stack lmao
		assert(md_len == 20);

		// Make sure it matches
		good_bits = 0;
#if defined(HAVE_FLSLL)
		// Nobody is going to ask for more than 64 bits
		assert(bits <= 64);
		good_bits = 64 - flsll(htonll(*(uint64_t *)&hash.id[0]));
#else
		int still_good = 1;
		for (int n = 0; n < bits; n++) {
			// 1 if the bit `n` is unset
			int bit = ((hash.id[n / 8] >> (7 - (n % 8))) & 1) ^ 1;
			// Will be 1 as long as no bits were set
			still_good &= bit;
			// Will add 1 as long as no bits were set
			good_bits += still_good;
		}
#endif
		// Only update atomics if we PB because speed
		if (good_bits > local_closest) {
			local_closest = good_bits;
			if (good_bits > closest_bits) {
				closest_bits = good_bits;
				closest = hash;
			}
		}

		// Update attempts every 0x100. Probably not a big impact but I am going to
		// micro-optimize every part of this.
		if ((nonce & 0xFF) == 0) {
			attempts += 0x100;
		}
		nonce++;
	} while (good_bits < bits);

	// Big sanity check here since we think this is a good hash
	git_oid hash;
	git_odb_hash(&hash, commit_buf, commit_size, GIT_OBJECT_COMMIT);
	int test = 0;
	for (size_t n = 0; n < bits; n++) {
		// Test will be the OR of the first `n` bits
		test |= (hash.id[n / 8] >> (7 - (n % 8))) & 1;
	}

	// Hopefully this never fails
	assert(test == 0);

	// Report results WHILE HOLDING THE LOCK
	{
		std::unique_lock<std::mutex> l(m);

		// If we are first
		if (!finished) {
			result_t local_result{};
			local_result.buf_size = commit_size;
			memcpy(local_result.buf, commit_buf, commit_size);

			result = local_result;
			success = true;
			finished = true;
			cv.notify_all();
		}
	}

	EVP_MD_CTX_free(md_ctx);
	EVP_MD_CTX_free(md_ctx_shared);
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

	// libgit2 init is nice
	git_libgit2_init();
	// OpenSSL init is not
	ERR_load_CRYPTO_strings();
	OpenSSL_add_all_algorithms();
	OPENSSL_no_config();

	// Repo in cwd
	char dir[0x400];
	getcwd(dir, 0x400);

	// Find our repo
	git_repository *repository;
	git_repository_open(&repository, dir);

	if (repository == nullptr) {
		fprintf(stderr, "Current directory is not a git repository.\n");
		// I could use a goto here... or I could just not
		git_libgit2_shutdown();

		EVP_cleanup();
		CRYPTO_cleanup_all_ex_data();
		ERR_free_strings();
		return 2;
	}

	git_reference *head;
	git_repository_head(&head, repository);

	if (head == nullptr) {
		fprintf(stderr, "Current repository does not have a HEAD.\n");
		// I could use a goto here... or I could just not
		git_repository_free(repository);
		git_libgit2_shutdown();

		EVP_cleanup();
		CRYPTO_cleanup_all_ex_data();
		ERR_free_strings();
		return 3;
	}

	const git_oid *target = git_reference_target(head);

	// Copy from the latest commit
	git_commit *commit;
	git_commit_lookup(&commit, repository, target);

	if (commit == nullptr) {
		fprintf(stderr, "No commit on current branch.\n");
		// I could use a goto here... or I could just not
		git_reference_free(head);
		git_repository_free(repository);
		git_libgit2_shutdown();

		EVP_cleanup();
		CRYPTO_cleanup_all_ex_data();
		ERR_free_strings();
		return 4;
	}

	// Read
	git_odb *db;
	git_repository_odb(&db, repository);

	git_odb_object *commit_data;
	git_odb_read(&commit_data, db, git_commit_id(commit));

#ifdef DEBUG
	fprintf(stderr, "OLD COMMIT:\n%s\n", (char *)git_odb_object_data(commit_data));
#endif

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
			try_commits(i, threads, bits, (char *)git_odb_object_data(commit_data),
			            git_odb_object_size(commit_data));
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

		// And make the commit for them
		git_oid hash;
		git_odb_write(&hash, db, commit_buffer, commit_size, GIT_OBJECT_COMMIT);

		// Fancy print
		char id[0x100];
		git_oid_tostr(id, 0x100, &hash);

#ifdef DEBUG
		fprintf(stderr, "NEW COMMIT:\n%s\n", commit_buffer);
#endif
		printf("\nFound commit hash %s\n", id);

		// Soft reset to this commit so it is now branch head
		git_object *new_commit;
		git_object_lookup(&new_commit, repository, &hash, GIT_OBJECT_COMMIT);

		git_reset(repository, new_commit, GIT_RESET_SOFT, nullptr);

		git_object_free(new_commit);
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
	git_odb_free(db);
	git_commit_free(commit);

	git_reference_free(head);
	git_repository_free(repository);

	git_libgit2_shutdown();

	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
	ERR_free_strings();

	return 0;
}

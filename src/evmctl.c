/*
 * evm-utils - IMA/EVM support utilities
 *
 * Copyright (C) 2011 Nokia Corporation
 * Copyright (C) 2011 Intel Corporation
 *
 * Authors:
 * Dmitry Kasatkin <dmitry.kasatkin@nokia.com>
 *                 <dmitry.kasatkin@intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * File: evmctl.c
 *	 IMA/EVM control program
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <attr/xattr.h>
#include <getopt.h>
#include <signal.h>
#include <keyutils.h>
#include <asm/byteorder.h>
#include <syslog.h>
#include <attr/xattr.h>

#include <openssl/sha.h>
#include <openssl/sha.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/hmac.h>

#define USE_FPRINTF

#ifdef USE_FPRINTF
#define do_log(level, fmt, args...)	if (level <= verbose) fprintf(stderr, fmt, ##args)
#define do_log_dump(level, p, len)	if (level <= verbose) do_dump(stderr, p, len)
#else
#define do_log(level, fmt, args...)	syslog(level, fmt, ##args)
#define do_log_dump(p, len)
#endif

#ifdef DEBUG
#define log_debug(fmt, args...)		do_log(LOG_DEBUG, "%s:%d " fmt, __func__ , __LINE__ , ##args)
#define log_debug_dump(p, len)		do_log_dump(LOG_DEBUG, p, len)
#else
#define log_debug(fmt, args...)
#define log_debug_dump(p, len)
#endif

#define log_dump(p, len)		do_log_dump(LOG_INFO, p, len)
#define log_info(fmt, args...)		do_log(LOG_INFO, fmt, ##args)
#define log_err(fmt, args...)		do_log(LOG_ERR, fmt, ##args)
#define log_errno(fmt, args...)		do_log(LOG_ERR, fmt ": %s (%d)\n", ##args, strerror(errno), errno)

#define	DATA_SIZE	4096
#define SHA1_HASH_LEN   20

#define	EXT2_IOC_GETVERSION	_IOR('v', 1, long)
#define	EXT34_IOC_GETVERSION	_IOR('f', 3, long)

#define	FS_IOC_GETFLAGS		_IOR('f', 1, long)
#define	FS_IOC_SETFLAGS		_IOW('f', 2, long)
#define FS_IOC32_GETFLAGS	_IOR('f', 1, int)
#define FS_IOC32_SETFLAGS	_IOW('f', 2, int)

struct h_misc {
	unsigned long ino;
	uint32_t generation;
	uid_t uid;
	gid_t gid;
	unsigned short mode;
} hmac_misc;

enum pubkey_algo {
	PUBKEY_ALGO_RSA,
	PUBKEY_ALGO_MAX,
};

enum digest_algo {
	DIGEST_ALGO_SHA1,
	DIGEST_ALGO_SHA256,
	DIGEST_ALGO_MAX
};

struct pubkey_hdr {
	uint8_t		version;	/* key format version */
	time_t		timestamp;	/* key made, always 0 for now */
	uint8_t		algo;
	uint8_t		nmpi;
	char		mpi[0];
} __attribute__ ((packed));


struct signature_hdr {
	uint8_t		version;	/* signature format version */
	time_t		timestamp;	/* signature made */
	uint8_t		algo;
	uint8_t		hash;
	uint8_t		keyid[8];
	uint8_t		nmpi;
	char		mpi[0];
} __attribute__ ((packed));


static char *evm_config_xattrnames[] = {
	"security.selinux",
	"security.SMACK64",
	"security.ima",
	"security.capability",
	NULL
};

struct command {
	char	*name;
	int	(*func)(struct command *cmd);
	int	cmd;
	char	*arg;
	char	*msg;	/* extra info message */
};

static int		verbose = LOG_INFO - 1;
static int		g_argc;
static char		**g_argv;
static int		set_xattr = 1;
static int		digest = 0;
static int		digsig = 0;
static char		*hash_algo = "sha1";
static int		binkey = 0;

extern struct command	cmds[];
static void print_usage(struct command *cmd);

static void do_dump(FILE *fp, const void *ptr, int len)
{
        int     i;
        uint8_t *data = (uint8_t *)ptr;

        for (i = 0; i < len; i++) {
                fprintf(fp, "%02x", data[i]);
        }
        fprintf(fp, "\n");
}

static void dump(const void *ptr, int len)
{
	do_dump(stdout, ptr, len);
}

static inline int get_filesize(const char *filename)
{
	struct stat stats;
	/*  Need to know the file length */
	stat(filename, &stats);
	return (int) stats.st_size;
}

static inline int get_fdsize(int fd)
{
	struct stat stats;
	/*  Need to know the file length */
	fstat(fd, &stats);
	return (int) stats.st_size;
}

static int bin2file(const char *file, const char *ext, const unsigned char *data, int len)
{
	FILE *fp;
	char name[strlen(file) + (ext ? strlen(ext) : 0) + 2];
	int err;
	
	if (ext)
		sprintf(name, "%s.%s", file, ext);
	else
		sprintf(name, "%s", file);

	log_info("Writing to %s\n", name);

	fp = fopen(name, "w");
	if (!fp) {
		log_errno("Unable to open %s for writing", name);
		return -1;
	}
	err = fwrite(data, len, 1, fp);
	fclose(fp);
	return err;
}

static char *file2bin(const char *file, int *size)
{
	FILE *fp;
	int len;
	char *data;
		
	len = get_filesize(file);
	fp = fopen(file, "r");
	if (!fp) {
		log_errno("Unable to open %s", file);
		return NULL;
	}
	data = malloc(len);
	if (!fread(data, len, 1, fp))
		len = 0;
	fclose(fp);
	
	*size = len;
	return data;	
}

/*
 * Create binary key representation suitable for kernel
 */
static int key2bin(RSA *key, unsigned char *pub)
{
	int len, b, offset = 0;
	struct pubkey_hdr *pkh = (struct pubkey_hdr *)pub;

	/* add key header */
	pkh->version = 1;
	pkh->timestamp = 0;	/* PEM has no timestamp?? */
	pkh->algo = PUBKEY_ALGO_RSA;
	pkh->nmpi = 2;
	
	offset += sizeof(*pkh);
	
	// MPIs
	len = BN_num_bytes(key->n);
	b = BN_num_bits(key->n);
	pub[offset++] = b >> 8;
	pub[offset++] = b & 0xff;
	BN_bn2bin(key->n, &pub[offset]);
	offset += len;
	
	len = BN_num_bytes(key->e);
	b = BN_num_bits(key->e);
	pub[offset++] = b >> 8;
	pub[offset++] = b & 0xff;
	BN_bn2bin(key->e, &pub[offset]);
	offset += len;
	
	return offset;
}

static int read_key(const char *inkey, unsigned char *pub)
{
	FILE *fp;
	RSA *key = NULL, *key1;
	int len;

	fp = fopen(inkey, "r");
	if (!fp) {
		log_errno("read key failed from file %s", inkey);
		return -1;
	}

	key1 = PEM_read_RSA_PUBKEY(fp, &key, NULL, NULL);
	fclose(fp);
	if (!key1) {
		log_errno("PEM_read_RSA_PUBKEY() failed");
		return -1;
	}
	
	len = key2bin(key, pub);
	
	RSA_free(key);
	
	return len;
}


static void calc_keyid(uint8_t *keyid, char *str, const unsigned char *pkey, int len)
{
	uint8_t sha1[SHA_DIGEST_LENGTH];
	uint64_t id;
	
	log_debug("pkey:\n");
	log_debug_dump(pkey, len);
	SHA1(pkey, len, sha1);	
	
	//sha1[12 - 19] is exactly keyid from gpg file
	memcpy(keyid, sha1 + 12, 8);
	log_debug("keyid:\n");
	log_debug_dump(keyid, 8);
	
	id = __be64_to_cpup((__be64 *)keyid);
	sprintf(str, "%llX", (unsigned long long)id);
	log_info("keyid: %s\n", str);	
}

static int sign_hash(const unsigned char *hash, int size, const char *keyfile, unsigned char *sig)
{
	int err, len;
	SHA_CTX ctx;
	unsigned char pub[1024];
	RSA *key = NULL, *key1;
	FILE *fp;
	char name[20];
	unsigned char sighash[20];
	struct signature_hdr *hdr = (struct signature_hdr *)sig;
	uint16_t *blen;
	
	log_info("hash: ");
	log_dump(hash, size);
	
	fp = fopen(keyfile, "r");	
	if (!fp) {
		log_errno("Unable to open keyfile %s", keyfile);
		return -1;
	}
	key1 = PEM_read_RSAPrivateKey(fp, &key, NULL, NULL);
	fclose(fp);
	if (!key1) {
		log_errno("RSAPrivateKey() failed");
		return -1;
	}

	/* now create a new hash */
	hdr->version = 1;
	time(&hdr->timestamp);
	hdr->algo = PUBKEY_ALGO_RSA;
	hdr->hash = DIGEST_ALGO_SHA1;
	
	len = key2bin(key, pub);
	calc_keyid(hdr->keyid, name, pub, len);
	
	hdr->nmpi = 1;
	
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, hash, size);
	SHA1_Update(&ctx, hdr, sizeof(*hdr));
	SHA1_Final(sighash, &ctx);
	log_info("sighash: ");
	log_dump(sighash, sizeof(sighash));

	err = RSA_private_encrypt(sizeof(sighash), sighash, sig + sizeof(*hdr) + 2, key, RSA_PKCS1_PADDING);
	RSA_free(key);
	if (err < 0) {
		log_errno("RSA_private_encrypt() failed: %d", err);
		return -1;
	}

	len = err;
	
	/* we add bit length of the signature to make it gnupg compatible */
	blen = (uint16_t *)(sig + sizeof(*hdr));
	*blen = __cpu_to_be16(len << 3);
	len += sizeof(*hdr) + 2;
	log_info("evm/ima signature: %d bytes\n", len);
	if (!set_xattr || verbose >= LOG_INFO)
		dump(sig, len);

	return len;	
}

static int calc_evm_hash(const char *file, const char *keyfile, unsigned char *hash)
{
	struct stat st;
	int fd, err;
	uint32_t generation;
	EVP_MD_CTX ctx;
	const EVP_MD *md;
	unsigned int mdlen;
	char **xattrname;
	char xattr_value[1024];
	
	fd = open(file, 0);
	if (fd < 0) {
		log_errno("Unable to open %s", file);
		return -1;
	}
	
	if (fstat(fd, &st)) {
		log_errno("fstat() failed");
		return -1;
	}
	
	if (ioctl(fd, EXT34_IOC_GETVERSION, &generation)) {
		log_errno("ioctl() failed");
		return -1;
	}
	
	close(fd);
	
	log_info("generation: %u\n", generation);

	OpenSSL_add_all_digests();

	md = EVP_get_digestbyname("sha1");
	if (!md) {
		log_errno("EVP_get_digestbyname() failed");
		return -1;
	}

	err = EVP_DigestInit(&ctx, md);
	if (!err) {
		log_errno("EVP_DigestInit() failed");
		return -1;
	}

	for (xattrname = evm_config_xattrnames; *xattrname != NULL; xattrname++) {
		err = getxattr(file, *xattrname, xattr_value, sizeof(xattr_value));
		if (err < 0) {
			log_info("no attr: %s\n", *xattrname);
			continue;
		}
		//log_debug("name: %s, value: %s, size: %d\n", *xattrname, xattr_value, err);
		log_info("name: %s, size: %d\n", *xattrname, err);
		log_debug_dump(xattr_value, err);
		err = EVP_DigestUpdate(&ctx, xattr_value, err);
		if (!err) {
			log_errno("EVP_DigestUpdate() failed");
			return -1;
		}
	}

	memset(&hmac_misc, 0, sizeof(hmac_misc));
	hmac_misc.ino = st.st_ino;
	hmac_misc.generation = generation;
	hmac_misc.uid = st.st_uid;
	hmac_misc.gid = st.st_gid;
	hmac_misc.mode = st.st_mode;
	
	err = EVP_DigestUpdate(&ctx, (const unsigned char*)&hmac_misc, sizeof(hmac_misc));
	if (!err) {
		log_errno("EVP_DigestUpdate() failed");
		return -1;
	}
	err = EVP_DigestFinal(&ctx, hash, &mdlen);
	if (!err) {
		log_errno("EVP_DigestFinal() failed");
		return -1;
	}
	
	return 0;
}

static int sign_evm(const char *file, const char *key)
{
	unsigned char hash[20];
	unsigned char sig[1024] = "\x03";
	int err;

	calc_evm_hash(file, key, hash);
	
	err = sign_hash(hash, sizeof(hash), key, sig + 1);
	if (err < 0)
		return err;
	
	if (set_xattr) {
		err = setxattr(file, "security.evm", sig, err + 1, 0);
		if (err < 0) {
			log_errno("setxattr failed: %s", file);
			return err;
		}
	}
	
	return 0;
}

static int calc_file_hash(const char *file, uint8_t *hash)
{
	EVP_MD_CTX ctx;
	const EVP_MD *md;
	uint8_t	*data;
	int err, size, bs = DATA_SIZE;
	size_t len;
	unsigned int mdlen;
	FILE *fp;

	data = malloc(bs);
	if (!data) {
		log_errno("malloc failed");
		return -1;
	}
		
	fp = fopen(file, "r");
	if (!fp) {
		log_errno("Unable to open %s", file);
		return -1;
	}
	
	OpenSSL_add_all_digests();

	md = EVP_get_digestbyname(hash_algo);
	if (!md) {
		log_errno("EVP_get_digestbyname() failed");
		return -1;
	}

	err = EVP_DigestInit(&ctx, md);
	if (!err) {
		log_errno("EVP_DigestInit() failed");
		return -1;
	}

	for (size = get_fdsize(fileno(fp)); size; size -= len) {
		len = MIN(size, bs);
		err = fread(data, len, 1, fp);
		if (!err) {
			if (ferror(fp)) {
				log_errno("fread() error\n");
				return -1;
			}
			break;
		}
		err = EVP_DigestUpdate(&ctx, data, len);
		if (!err) {
			log_errno("EVP_DigestUpdate() failed");
			return -1;
		}
	}

	err = EVP_DigestFinal(&ctx, hash, &mdlen);
	if (!err) {
		log_errno("EVP_DigestFinal() failed");
		return -1;
	}

	fclose(fp);
	
	free(data);
	
	return mdlen;
}

static int hash_ima(const char *file)
{
	unsigned char hash[65] = "\x01";// MAX hash size + 1
	int err;

	err = calc_file_hash(file, hash + 1);
	if (err < 0)
		return err;
	
	if (!set_xattr || verbose >= LOG_INFO)
		dump(hash, err + 1);

	if (set_xattr) {
		err = setxattr(file, "security.ima", hash, err + 1, 0);
		if (err < 0) {
			log_errno("setxattr failed: %s", file);
			return err;
		}
	}
	
	return 0;
}

static int cmd_hash_ima(struct command *cmd)
{
	char *file = g_argv[optind++];

	if (!file) {
		log_err("Parameters missing\n");
		print_usage(cmd);
		return 1;
	}
	
	return hash_ima(file);
}

static int sign_ima(const char *file, const char *key)
{
	unsigned char hash[64];
	unsigned char sig[1024] = "\x03";
	int err;
	
	err = calc_file_hash(file, hash);
	if (err < 0)
		return err;
	
	err = sign_hash(hash, err, key, sig + 1);
	if (err < 0)
		return err;
	
	if (set_xattr) {
		err = setxattr(file, "security.ima", sig, err + 1, 0);
		if (err < 0) {
			log_errno("setxattr failed: %s", file);
			return err;
		}
	}
	
	return 0;
}

static int cmd_sign_ima(struct command *cmd)
{
	char *key, *file = g_argv[optind++];
	
	if (!file) {
		log_err("Parameters missing\n");
		print_usage(cmd);
		return 1;
	}
	
	key = g_argv[optind++];
	if (!key)
		key = "/etc/keys/privkey_evm.pem";
	
	return sign_ima(file, key);
	
}

static int cmd_sign_evm(struct command *cmd)
{
	char *key, *file = g_argv[optind++];
	int err;
	
	if (!file) {
		log_err("Parameters missing\n");
		print_usage(cmd);
		return 1;
	}
	
	key = g_argv[optind++];
	if (!key)
		key = "/etc/keys/privkey_evm.pem";
	
	if (digsig) {
		err = sign_ima(file, key);
		if (err)
			return err;
	}
	
	if (digest) {
		err = hash_ima(file);
		if (err)
			return err;
	}
	
	return sign_evm(file, key);
}

static int verify_hash(const unsigned char *hash, int size, unsigned char *sig, int siglen, const char *keyfile)
{
	int err, len;
	SHA_CTX ctx;
	unsigned char out[1024];
	RSA *key = NULL, *key1;
	FILE *fp;
	unsigned char sighash[20];
	struct signature_hdr *hdr = (struct signature_hdr *)sig;
	
	log_info("hash: ");
	log_dump(hash, size);
	
	fp = fopen(keyfile, "r");	
	if (!fp) {
		log_errno("Unable to open keyfile %s", keyfile);
		return -1;
	}
	key1 = PEM_read_RSA_PUBKEY(fp, &key, NULL, NULL);
	fclose(fp);
	if (!key1) {
		log_errno("PEM_read_RSA_PUBKEY() failed");
		return -1;
	}

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, hash, size);
	SHA1_Update(&ctx, hdr, sizeof(*hdr));
	SHA1_Final(sighash, &ctx);
	log_info("sighash: ");
	log_dump(sighash, sizeof(sighash));

	err = RSA_public_decrypt(siglen - sizeof(*hdr) - 2, sig + sizeof(*hdr) + 2, out, key, RSA_PKCS1_PADDING);
	RSA_free(key);
	if (err < 0) {
		log_errno("RSA_public_decrypt() failed: %d", err);
		return -1;
	}

	len = err;
	
	if (len != sizeof(sighash) || memcmp(out, sighash, len) != 0) {
		log_errno("Verification failed: %d", err);
		return -1;
	} else {
		//log_info("Verification is OK\n");
		printf("Verification is OK\n");
	}
	
	return 0;
}

static int verify_evm(const char *file, const char *key)
{
	unsigned char hash[20];
	unsigned char sig[1024];
	int err;

	calc_evm_hash(file, key, hash);
	
	err = getxattr(file, "security.evm", sig, sizeof(sig));
	if (err < 0) {
		log_errno("getxattr failed");
		return err;
	}
	
	if (sig[0] != 0x03) {
		log_errno("security.evm has not signature");
		return err;
	}
	
	return verify_hash(hash, sizeof(hash), sig + 1, err - 1, key);
}

static int cmd_verify_evm(struct command *cmd)
{
	char *key, *file = g_argv[optind++];
	
	if (!file) {
		log_err("Parameters missing\n");
		print_usage(cmd);
		return 1;
	}
	
	key = g_argv[optind++];
	if (!key)
		key = "/etc/keys/pubkey_evm.pem";
	
	return verify_evm(file, key);
}

static int cmd_convert(struct command *cmd)
{
	char *inkey, *outkey = NULL;
	unsigned char pub[1024];
	char name[20];
	int len;
	uint8_t keyid[8];
	
	inkey = g_argv[optind++];
	if (!inkey)
		inkey = "/etc/keys/pubkey_evm.pem";
	else
		outkey = g_argv[optind++];
		
	if (!outkey)
		outkey = "pubkey_evm.bin";

	log_info("Convert public key %s to %s\n", inkey, outkey);	

	len = read_key(inkey, pub);
	if (len < 0)
		return -1;
	
	calc_keyid(keyid, name, pub, len);
	
	bin2file(outkey, name, pub, len);

	return 0;
}

static int cmd_import_bin(struct command *cmd)
{
	int len;
	char *inkey, *ring = NULL;
	char *key, name[20];
	key_serial_t id;
	uint8_t keyid[8];
	
	inkey = g_argv[optind++];
	if (!inkey)
		inkey = "/etc/keys/pubkey_evm.bin";
 	else
 		ring = g_argv[optind++];
 		
 	if (!ring)
 		id = KEY_SPEC_USER_KEYRING;
	else
		id = atoi(ring);

	key = file2bin(inkey, &len);
	if (!key)
		return -1;
	
	calc_keyid(keyid, name, (unsigned char *)key, len);

 	log_info("Importing public key %s from file %s into keyring %d\n", name, inkey, id);
	
	id = add_key("user", name, key, len, id);
	if (id < 0) {
		log_errno("add_key failed");
		return -1;
	}

	log_info("keyid: %d\n", id);
	printf("%d\n", id);
	
	free(key);
	
	return 0;
}

static int cmd_import(struct command *cmd)
{
	char *inkey, *ring = NULL;
	unsigned char key[1024];
	int id, len;
	char name[20];
	uint8_t keyid[8];

	if (binkey)
		return cmd_import_bin(cmd);
	
	inkey = g_argv[optind++];
	if (!inkey)
		inkey = "/etc/keys/pubkey_evm.pem";
 	else
 		ring = g_argv[optind++];
 		
 	if (!ring)
 		id = KEY_SPEC_USER_KEYRING;
	else
		id = atoi(ring);

	len = read_key(inkey, key);
	if (len < 0)
		return -1;
	
	calc_keyid(keyid, name, key, len);
	
 	log_info("Importing public key %s from file %s into keyring %d\n", name, inkey, id);
	
	id = add_key("user", name, key, len, id);
	if (id < 0) {
		log_errno("add_key failed");
		return -1;
	}

	log_info("keyid: %d\n", id);
	printf("%d\n", id);
	
	return 0;
}

#define MAX_KEY_SIZE 128

static int calc_evm_hmac(const char *file, const char *keyfile, unsigned char *hash)
{
	struct stat st;
	int fd, err;
	uint32_t generation;
	HMAC_CTX ctx;
	unsigned int mdlen;
	char **xattrname;
	unsigned char xattr_value[1024];
	char *key;
	int keylen;
	unsigned char evmkey[MAX_KEY_SIZE];
	
	key = file2bin(keyfile, &keylen);
	if (!key) {
		log_errno("Unable to read a key: %s\n", keyfile);
		return -1;
	}
	
	if (keylen > sizeof(evmkey)) {
		log_errno("key is too long\n");
		return -1;
	}

	/* EVM key is 128 bytes */
	memcpy(evmkey, key, keylen);
	memset(evmkey + keylen, 0, sizeof(evmkey) - keylen);
	
	fd = open(file, 0);
	if (fd < 0) {
		log_errno("Unable to open %s", file);
		return -1;
	}
	
	if (fstat(fd, &st)) {
		log_errno("fstat() failed");
		return -1;
	}
	
	if (ioctl(fd, EXT34_IOC_GETVERSION, &generation)) {
		log_errno("ioctl() failed");
		return -1;
	}
	
	close(fd);
	
	log_info("generation: %u\n", generation);

	HMAC_Init(&ctx, evmkey, sizeof(evmkey), EVP_sha1());

	for (xattrname = evm_config_xattrnames; *xattrname != NULL; xattrname++) {
		err = getxattr(file, *xattrname, xattr_value, sizeof(xattr_value));
		if (err < 0) {
			log_info("no attr: %s\n", *xattrname);
			continue;
		}
		//log_debug("name: %s, value: %s, size: %d\n", *xattrname, xattr_value, err);
		log_info("name: %s, size: %d\n", *xattrname, err);
		log_debug_dump(xattr_value, err);
		HMAC_Update(&ctx, xattr_value, err);
	}

	memset(&hmac_misc, 0, sizeof(hmac_misc));
	hmac_misc.ino = st.st_ino;
	hmac_misc.generation = generation;
	hmac_misc.uid = st.st_uid;
	hmac_misc.gid = st.st_gid;
	hmac_misc.mode = st.st_mode;
	
	HMAC_Update(&ctx, (const unsigned char*)&hmac_misc, sizeof(hmac_misc));
	HMAC_Final(&ctx, hash, &mdlen);
	HMAC_CTX_cleanup(&ctx);
	
	free(key);

	return 0;
}

static int hmac_evm(const char *file, const char *key)
{
	unsigned char hash[20];
	unsigned char sig[1024] = "\x02";
	int err;

	calc_evm_hmac(file, key, hash);
	
	log_info("hmac: ");
	log_dump(hash, sizeof(hash));
	memcpy(sig + 1, hash, sizeof(hash));
	err = sizeof(hash);
	
	if (set_xattr) {
		err = setxattr(file, "security.evm", sig, err + 1, 0);
		if (err < 0) {
			log_errno("setxattr failed: %s", file);
			return err;
		}
	}
	
	return 0;
}

static int cmd_hmac_evm(struct command *cmd)
{
	char *key, *file = g_argv[optind++];
	int err;
	
	if (!file) {
		log_err("Parameters missing\n");
		print_usage(cmd);
		return 1;
	}
	
	key = g_argv[optind++];
	if (!key)
		key = "/etc/keys/privkey_evm.pem";
	
	if (digsig) {
		err = sign_ima(file, key);
		if (err)
			return err;
	}
	
	if (digest) {
		err = hash_ima(file);
		if (err)
			return err;
	}
	
	return hmac_evm(file, "/etc/keys/evm-key-plain");
}

static void print_usage(struct command *cmd)
{
	printf("usage: %s %s\n", cmd->name, cmd->arg ? cmd->arg : "");
}

static void print_full_usage(struct command *cmd)
{
	if (cmd->name)
		printf("usage: %s %s\n", cmd->name, cmd->arg ? cmd->arg : "");
	if (cmd->msg)
		printf("description:\n%s", cmd->msg);

}

static int print_command_usage(struct command *cmds, char *command)
{
	struct command	*cmd;

	for (cmd = cmds; cmd->name; cmd++) {
		if (strcmp(cmd->name, command) == 0) {
			print_full_usage(cmd);
			return 0;
		}
	}
	printf("invalid command: %s\n", command);
	return 	1;
}

static void print_all_usage(struct command *cmds)
{
	struct command	*cmd;
	
	for (cmd = cmds; cmd->name; cmd++) {
		if (cmd->arg)
			printf("%s %s\n", cmd->name, cmd->arg);
		else if (cmd->msg)
			printf("%s", cmd->msg);
	}
}

static int call_command(struct command *cmds, char *command)
{
	struct command	*cmd;
	
	for (cmd = cmds; cmd->name; cmd++) {
		if (strcasecmp(cmd->name, command) == 0)
			return cmd->func(cmd);
	}
	printf("Invalid command: %s\n", command);
	return -1;
}

static int cmd_help(struct command *cmd)
{
	if (!g_argv[optind]) {
		print_usage(cmd);
		return 0;
	} else
		return print_command_usage(cmds, g_argv[optind]);
}

static void usage(void)
{
	printf("Usage: evmctl <command> [parameters..]\n");

	print_all_usage(cmds);
}

struct command cmds[] = {
	{"help", cmd_help, 0, "<command>"},
	{"import", cmd_import, 0, "[--bin] inkey keyring", "Import public key (PEM/bin) into the keyring.\n" },
	{"convert", cmd_convert, 0, "inkey outkey", "Convert PEM public key into IMA/EVM kernel friendly format.\n" },
	{"sign", cmd_sign_evm, 0, "[--imahash | --imasig ] file [key]", "Sign file metadata.\n" },
	{"verify", cmd_verify_evm, 0, "file", "Verify EVM.\n" },
	{"ima_sign", cmd_sign_ima, 0, "file [key]", "Sign file content.\n" },
	{"ima_hash", cmd_hash_ima, 0, "file", "Hash file content.\n" },
	{"hmac", cmd_hmac_evm, 0, "[--imahash | --imasig ] file [key]", "Sign file metadata with HMAC (for debugging).\n" },
	{0, 0, 0, NULL}
};

static struct option  opts[] = {
	{"help", 0, 0, 'h'},
	{"inkey", 1, 0, 'k'},
	{"imasig", 0, 0, 's'},
	{"imahash", 0, 0, 'd'},
	{"hashalgo", 1, 0, 'a'},
	{"bin", 0, 0, 'b'},
	{}

};

int main(int argc, char *argv[])
{
	int err = 0, c, lind;

	g_argv = argv;
	g_argc = argc;

	while (1) {
		c = getopt_long(argc, argv, "hk:vnsda:b", opts, &lind);
		if (c == -1)
			break;
			
		switch (c) {
		case 'h':
			usage();
			exit(0);
			break;
		case 'k':
			printf("inkey: %s\n", optarg);
			break;
		case 'v':
			verbose++;
			break;
		case 'd':
			digest = 1;
			break;
		case 's':
			digsig = 1;
			break;
		case 'n':
			set_xattr = 0; // do not set Extended Attributes... just print signature
			break;
		case 'a':
			hash_algo = optarg;
			break;
		case 'b':
			binkey = 1;
			break;
		case '?':
			exit(1);
			break;
		default:
			log_err("getopt() returned: %d (%c)\n", c, c);
		}
	}

	if (argv[optind] == NULL)
		usage();
	else
		err = call_command(cmds, argv[optind++]);

	return err;
}
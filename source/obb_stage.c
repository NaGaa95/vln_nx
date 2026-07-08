/* obb_stage.c -- first-boot, on-device staging of the Very Little Nightmares OBB.
 *
 * VLN is a Unity "split application binary": the APK ships a small data.unity3d STUB
 * and a ~529MB OBB (a STORE/uncompressed ZIP) whose data.unity3d holds the content
 * half. Neither boots alone. This unpacks the OBB flat and merges the two
 * data.unity3d halves on-device (what the PC tools/stage_sd.py does), keeping every
 * data block verbatim and rebuilding only the uncompressed blocksInfo -- so we need
 * an LZ4 decompressor for the inputs, never a compressor. Paths are CWD-relative.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>          /* chdir, unlink */

#ifdef OBB_STAGE_PC_TEST
  #define debugPrintf printf
#else
  #include "util.h"          /* debugPrintf */
#endif
#define OBB_MKDIR(p) mkdir((p), 0777)
#include "obb_stage.h"

/* One-time install screen + clock boost (a blank screen looks like a hang; the boost
 * makes the ~500MB copy run as fast as the SD allows). No-ops in the PC test. */
#ifndef OBB_STAGE_PC_TEST
#include <switch.h>
static void ui_begin(unsigned long long mb){
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  consoleInit(NULL);
  printf("\x1b[2J\x1b[3;5HVery Little Nightmares"
         "\x1b[5;5HInstalling game data (%llu MB) - first run only."
         "\x1b[6;5HThis takes about a minute; please wait.", mb);
  consoleUpdate(NULL);
}
static void ui_progress(int pct){ printf("\x1b[8;5HProgress: %3d %%", pct); consoleUpdate(NULL); }
static void ui_end(void){
  printf("\x1b[10;5HDone. Launching...");
  consoleUpdate(NULL);
  consoleExit(NULL);
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}
#else
static void ui_begin(unsigned long long mb){ (void)mb; }
static void ui_progress(int pct){ (void)pct; }
static void ui_end(void){ }
#endif

/* ---- endian helpers ---------------------------------------------------- */
static uint16_t be16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t be64(const uint8_t *p){ return ((uint64_t)be32(p)<<32)|be32(p+4); }
static void wbe32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static void wbe16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void wbe64(uint8_t *p, uint64_t v){ wbe32(p,(uint32_t)(v>>32)); wbe32(p+4,(uint32_t)v); }
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }
static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* ---- minimal LZ4 block decompressor (for the small blocksInfo) --------- */
static int lz4_block_decompress(const uint8_t *src, int slen, uint8_t *dst, int dcap){
  int s=0, d=0;
  while (s < slen){
    uint8_t tok = src[s++];
    int lit = tok >> 4;
    if (lit == 15){ int b; do { if (s>=slen) return -1; b=src[s++]; lit+=b; } while (b==255); }
    if (d+lit > dcap || s+lit > slen) return -1;
    memcpy(dst+d, src+s, lit); d+=lit; s+=lit;
    if (s >= slen) break;                       /* last sequence: literals only */
    if (s+2 > slen) return -1;
    int off = src[s] | (src[s+1]<<8); s+=2;
    if (off == 0 || d-off < 0) return -1;
    int ml = tok & 15;
    if (ml == 15){ int b; do { if (s>=slen) return -1; b=src[s++]; ml+=b; } while (b==255); }
    ml += 4;                                     /* minmatch */
    if (d+ml > dcap) return -1;
    int m = d-off;
    for (int i=0;i<ml;i++) dst[d+i] = dst[m+i];  /* overlapping copy */
    d += ml;
  }
  return d;
}

/* ---- streamed byte-range copy with progress ---------------------------- */
static uint64_t g_done, g_total, g_logged;
static int copy_range(FILE *src, long off, uint64_t len, FILE *dst){
  static uint8_t buf[1u<<20];
  if (fseek(src, off, SEEK_SET)) return -1;
  while (len){
    size_t n = len < sizeof buf ? (size_t)len : sizeof buf;
    if (fread(buf,1,n,src) != n) return -1;
    if (fwrite(buf,1,n,dst) != n) return -1;
    len -= n; g_done += n;
    if (g_total && g_done - g_logged >= 16u*1024*1024){
      g_logged = g_done;
      ui_progress((int)(g_done * 100 / g_total));
    }
  }
  return 0;
}
static int copy_file(const char *sp, const char *dp){
  FILE *s=fopen(sp,"rb"); if(!s) return -1;
  FILE *d=fopen(dp,"wb"); if(!d){ fclose(s); return -1; }
  fseek(s,0,SEEK_END); long n=ftell(s); fseek(s,0,SEEK_SET);
  int rc = copy_range(s,0,(uint64_t)n,d);
  fclose(s); fclose(d); return rc;
}

/* create every parent directory of a (relative) file path */
static void mkdir_parents(const char *filepath){
  char tmp[1024];
  strncpy(tmp, filepath, sizeof tmp - 1); tmp[sizeof tmp -1]=0;
  for (char *s = tmp+1; *s; s++){
    if (*s == '/'){ *s = 0; OBB_MKDIR(tmp); *s = '/'; }
  }
}

/* ---- UnityFS archive ---------------------------------------------------- */
typedef struct { uint32_t unc, comp; uint16_t flags; } UBlock;
typedef struct { int64_t off, size; uint32_t flags; char name[256]; } UNode;
typedef struct {
  uint32_t ver, comp, unc, flags;
  char uver[32], urev[32];
  int nblocks, nnodes;
  UBlock *blocks; UNode *nodes;
  uint64_t total_comp, total_unc;
  long blocks_raw_off;                 /* absolute file offset of the blocks region */
} UFS;

static void ufs_free(UFS *a){ free(a->blocks); free(a->nodes); a->blocks=NULL; a->nodes=NULL; }

/* Parse a UnityFS whose header starts at absolute offset `base` in f. */
static int ufs_parse(FILE *f, long base, UFS *a){
  memset(a, 0, sizeof *a);
  uint8_t h[128];
  if (fseek(f, base, SEEK_SET)) return -1;
  if (fread(h,1,sizeof h,f) != sizeof h) return -1;
  if (memcmp(h, "UnityFS", 8) != 0) return -1;      /* 7 chars + NUL */
  int o = 8;
  a->ver = be32(h+o); o += 4;
  size_t ul = strlen((char*)h+o); if (ul >= sizeof a->uver) return -1;
  memcpy(a->uver, h+o, ul+1); o += (int)ul + 1;
  size_t rl = strlen((char*)h+o); if (rl >= sizeof a->urev) return -1;
  memcpy(a->urev, h+o, rl+1); o += (int)rl + 1;
  o += 8;                                            /* skip size */
  a->comp = be32(h+o); o += 4;
  a->unc  = be32(h+o); o += 4;
  a->flags= be32(h+o); o += 4;
  long hdr_end = o;
  if (a->flags & 0x80) return -2;                    /* blocksInfoAtEnd: not used by VLN */
  long bi_off = (a->flags & 0x200) ? ((hdr_end + 15) & ~15L) : hdr_end;

  uint8_t *cbi = (uint8_t*)malloc(a->comp);
  uint8_t *raw = (uint8_t*)malloc(a->unc);
  if (!cbi || !raw){ free(cbi); free(raw); return -1; }
  if (fseek(f, base + bi_off, SEEK_SET) || fread(cbi,1,a->comp,f) != a->comp){ free(cbi); free(raw); return -1; }
  int cm = a->flags & 0x3f;
  if (cm == 2 || cm == 3){
    if (lz4_block_decompress(cbi, (int)a->comp, raw, (int)a->unc) != (int)a->unc){ free(cbi); free(raw); return -1; }
  } else if (cm == 0){
    if (a->comp != a->unc){ free(cbi); free(raw); return -1; }
    memcpy(raw, cbi, a->unc);
  } else { free(cbi); free(raw); return -3; }
  free(cbi);

  int p = 16;                                        /* skip uncompressedDataHash */
  a->nblocks = (int)be32(raw+p); p += 4;
  a->blocks = (UBlock*)malloc(sizeof(UBlock) * (a->nblocks>0?a->nblocks:1));
  if (!a->blocks){ free(raw); return -1; }
  for (int i=0;i<a->nblocks;i++){
    a->blocks[i].unc  = be32(raw+p); p += 4;
    a->blocks[i].comp = be32(raw+p); p += 4;
    a->blocks[i].flags= be16(raw+p); p += 2;
    a->total_unc  += a->blocks[i].unc;
    a->total_comp += a->blocks[i].comp;
  }
  a->nnodes = (int)be32(raw+p); p += 4;
  a->nodes = (UNode*)malloc(sizeof(UNode) * (a->nnodes>0?a->nnodes:1));
  if (!a->nodes){ free(raw); free(a->blocks); return -1; }
  for (int i=0;i<a->nnodes;i++){
    a->nodes[i].off  = (int64_t)be64(raw+p); p += 8;
    a->nodes[i].size = (int64_t)be64(raw+p); p += 8;
    a->nodes[i].flags= be32(raw+p); p += 4;
    int nl = 0; while (raw[p+nl]) nl++;
    if (nl >= (int)sizeof a->nodes[i].name){ free(raw); ufs_free(a); return -1; }
    memcpy(a->nodes[i].name, raw+p, nl); a->nodes[i].name[nl] = 0;
    p += nl + 1;
  }
  free(raw);

  long blk_off = bi_off + (long)a->comp;
  if (a->flags & 0x200) blk_off = (blk_off + 15) & ~15L;
  a->blocks_raw_off = base + blk_off;
  return 0;
}

/* Merge stub A + content B into out_path (UnityFS with UNCOMPRESSED blocksInfo). */
static int ufs_merge_write(const char *out_path, FILE *fa, UFS *A, FILE *fb, UFS *B){
  int nblocks = A->nblocks + B->nblocks;
  int nnodes  = A->nnodes  + B->nnodes;
  size_t bi_cap = 16 + 4 + (size_t)nblocks*10 + 4;
  for (int i=0;i<A->nnodes;i++) bi_cap += 20 + strlen(A->nodes[i].name) + 1;
  for (int i=0;i<B->nnodes;i++) bi_cap += 20 + strlen(B->nodes[i].name) + 1;
  uint8_t *bi = (uint8_t*)calloc(1, bi_cap);         /* 16-byte hash left as zeros */
  if (!bi) return -1;
  size_t q = 16;
  wbe32(bi+q,(uint32_t)nblocks); q+=4;
  for (int i=0;i<A->nblocks;i++){ wbe32(bi+q,A->blocks[i].unc);q+=4; wbe32(bi+q,A->blocks[i].comp);q+=4; wbe16(bi+q,A->blocks[i].flags);q+=2; }
  for (int i=0;i<B->nblocks;i++){ wbe32(bi+q,B->blocks[i].unc);q+=4; wbe32(bi+q,B->blocks[i].comp);q+=4; wbe16(bi+q,B->blocks[i].flags);q+=2; }
  wbe32(bi+q,(uint32_t)nnodes); q+=4;
  for (int i=0;i<A->nnodes;i++){ wbe64(bi+q,(uint64_t)A->nodes[i].off);q+=8; wbe64(bi+q,(uint64_t)A->nodes[i].size);q+=8; wbe32(bi+q,A->nodes[i].flags);q+=4; size_t nl=strlen(A->nodes[i].name); memcpy(bi+q,A->nodes[i].name,nl);q+=nl; bi[q++]=0; }
  for (int i=0;i<B->nnodes;i++){ int64_t off=B->nodes[i].off+(int64_t)A->total_unc; wbe64(bi+q,(uint64_t)off);q+=8; wbe64(bi+q,(uint64_t)B->nodes[i].size);q+=8; wbe32(bi+q,B->nodes[i].flags);q+=4; size_t nl=strlen(B->nodes[i].name); memcpy(bi+q,B->nodes[i].name,nl);q+=nl; bi[q++]=0; }
  size_t bi_len = q;

  uint8_t hdr[64]; size_t ho=0;
  memcpy(hdr+ho,"UnityFS",7); ho+=7; hdr[ho++]=0;
  wbe32(hdr+ho,A->ver); ho+=4;
  size_t ul=strlen(A->uver); memcpy(hdr+ho,A->uver,ul); ho+=ul; hdr[ho++]=0;
  size_t rl=strlen(A->urev); memcpy(hdr+ho,A->urev,rl); ho+=rl; hdr[ho++]=0;
  size_t size_pos = ho; ho+=8;                       /* size field, filled below */
  wbe32(hdr+ho,(uint32_t)bi_len); ho+=4;             /* compressedBlocksInfoSize (stored) */
  wbe32(hdr+ho,(uint32_t)bi_len); ho+=4;             /* uncompressedBlocksInfoSize */
  wbe32(hdr+ho,0x240); ho+=4;                        /* flags: combined | padding, compression 0 */

  size_t bi_start  = (ho + 15) & ~(size_t)15;
  size_t blk_start = (bi_start + bi_len + 15) & ~(size_t)15;
  uint64_t total   = blk_start + A->total_comp + B->total_comp;
  wbe64(hdr+size_pos, total);

  FILE *o = fopen(out_path,"wb");
  if (!o){ free(bi); return -1; }
  uint8_t zero[16] = {0};
  int rc = -1;
  if (fwrite(hdr,1,ho,o) != ho) goto done;
  if (bi_start > ho && fwrite(zero,1,bi_start-ho,o) != bi_start-ho) goto done;
  if (fwrite(bi,1,bi_len,o) != bi_len) goto done;
  { size_t pad = blk_start - (bi_start + bi_len); if (pad && fwrite(zero,1,pad,o) != pad) goto done; }
  if (copy_range(fa, A->blocks_raw_off, A->total_comp, o)) goto done;
  if (copy_range(fb, B->blocks_raw_off, B->total_comp, o)) goto done;
  rc = 0;
done:
  fclose(o); free(bi);
  return rc;
}

/* ---- ZIP (STORE) enumeration + flat extraction ------------------------- */
/* Walk the OBB's central directory. For each entry: if it is data.unity3d, record
 * its data offset (for the merge); otherwise copy its STORED bytes flat to disk. */
static int obb_unpack(const char *obb_path, FILE **out_z, long *u3d_off){
  *u3d_off = -1;
  FILE *z = fopen(obb_path,"rb");
  if (!z){ debugPrintf("[obb] cannot open %s\n", obb_path); return -1; }
  fseek(z,0,SEEK_END); long zsize = ftell(z);
  long scan = zsize > 66000 ? 66000 : zsize;
  uint8_t *tail = (uint8_t*)malloc(scan);
  if (!tail){ fclose(z); return -1; }
  fseek(z, zsize - scan, SEEK_SET);
  if (fread(tail,1,scan,z) != (size_t)scan){ free(tail); fclose(z); return -1; }
  long eocd = -1;
  for (long i = scan - 22; i >= 0; i--) if (le32(tail+i) == 0x06054b50){ eocd = i; break; }
  if (eocd < 0){ debugPrintf("[obb] no EOCD (not a zip?)\n"); free(tail); fclose(z); return -1; }
  uint32_t cd_count  = le16(tail+eocd+10);
  uint32_t cd_size   = le32(tail+eocd+12);
  uint32_t cd_offset = le32(tail+eocd+16);
  free(tail);

  uint8_t *cd = (uint8_t*)malloc(cd_size);
  if (!cd){ fclose(z); return -1; }
  fseek(z, cd_offset, SEEK_SET);
  if (fread(cd,1,cd_size,z) != cd_size){ free(cd); fclose(z); return -1; }

  int rc = 0; uint32_t off = 0;
  for (uint32_t i=0;i<cd_count;i++){
    if (off + 46 > cd_size || le32(cd+off) != 0x02014b50){ rc = -1; break; }
    uint16_t method = le16(cd+off+10);
    uint32_t csz    = le32(cd+off+20);
    uint16_t nl     = le16(cd+off+28);
    uint16_t el     = le16(cd+off+30);
    uint16_t cl     = le16(cd+off+32);
    uint32_t lho    = le32(cd+off+42);
    char name[512];
    uint16_t cpy = nl < sizeof name -1 ? nl : (uint16_t)(sizeof name -1);
    memcpy(name, cd+off+46, cpy); name[cpy]=0;
    off += 46u + nl + el + cl;
    if (nl == 0 || name[nl-1] == '/') continue;      /* directory entry */
    if (method != 0){ debugPrintf("[obb] '%s' compressed (method %u) -- unsupported\n", name, method); rc=-1; break; }

    /* local header -> data offset */
    uint8_t lh[30];
    if (fseek(z, lho, SEEK_SET) || fread(lh,1,30,z)!=30 || le32(lh)!=0x04034b50){ rc=-1; break; }
    long data_off = (long)lho + 30 + le16(lh+26) + le16(lh+28);

    /* the OBB's data.unity3d is the content half -> merged separately, not flat */
    size_t L = strlen(name);
    if (L >= 12 && strcmp(name + L - 12, "data.unity3d") == 0){ *u3d_off = data_off; continue; }

    mkdir_parents(name);
    FILE *of = fopen(name,"wb");
    if (!of){ debugPrintf("[obb] cannot write %s\n", name); rc=-1; break; }
    int e = copy_range(z, data_off, csz, of);
    fclose(of);
    if (e){ debugPrintf("[obb] I/O error writing %s (SD full?)\n", name); rc=-1; break; }
  }
  free(cd);
  if (rc){ fclose(z); return rc; }
  *out_z = z;                                        /* left open: merge reads the OBB u3d in place */
  return 0;
}

/* Preserve the boot stub, unpack the OBB's assets, merge the two data.unity3d halves,
 * commit the result, and drop the OBB + stub. Crash-safe: a saved stub is reused. */
static int obb_install(const char *obb_name){
  const char *U3D="assets/bin/Data/data.unity3d";
  const char *STUB="assets/bin/Data/data.unity3d.apkstub";
  const char *MERGED="assets/bin/Data/data.unity3d.merged";
  struct stat st;
  if (stat(STUB,&st) != 0){
    if (stat(U3D,&st) != 0) return -1;
    if (copy_file(U3D, STUB) != 0) return -1;
  }
  FILE *z = NULL; long u3d_off = -1;
  if (obb_unpack(obb_name, &z, &u3d_off) != 0 || u3d_off < 0){ if (z) fclose(z); return -1; }
  FILE *fs = fopen(STUB,"rb");
  if (!fs){ fclose(z); return -1; }
  UFS A, B;
  if (ufs_parse(fs, 0, &A) || ufs_parse(z, u3d_off, &B)){ fclose(fs); fclose(z); return -1; }
  int mrc = ufs_merge_write(MERGED, fs, &A, z, &B);
  ufs_free(&A); ufs_free(&B);
  fclose(fs); fclose(z);
  if (mrc){ unlink(MERGED); return -1; }
  unlink(U3D);
  if (rename(MERGED, U3D) != 0) return -1;
  unlink(STUB); unlink(obb_name);
  return 0;
}

int obb_stage(const char *obb_name){
  struct stat st;
  if (stat("assets/bin/Data/data.unity3d",&st)==0 && st.st_size > 100*1024*1024){
    unlink("assets/bin/Data/data.unity3d.apkstub"); unlink(obb_name);
    return 0;                                          /* already staged */
  }
  if (stat(obb_name,&st) != 0) return -1;              /* no data and no OBB */
  g_total = (uint64_t)st.st_size; g_done = g_logged = 0;
  ui_begin((unsigned long long)(g_total>>20));
  int rc = obb_install(obb_name);
  ui_end();
  return rc;
}

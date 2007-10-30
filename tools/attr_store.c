#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "attr_store.h"
#include "pool.h"
#include "repo.h"
#include "util.h"

#include "attr_store_p.h"

#define NAME_WIDTH 12
#define TYPE_WIDTH (16-NAME_WIDTH)
typedef union
{
  struct {
    unsigned short name : NAME_WIDTH;
    unsigned short type : TYPE_WIDTH;
  } nt;
  unsigned short as_short;
} NameType;

typedef struct
{
  NameType n;
  char val[0];
} __attribute__((packed)) NameVal;

#define STRINGSPACE_BLOCK 1023
#define STRING_BLOCK 127
#define LOCALID_NULL  0
#define LOCALID_EMPTY 1

Attrstore *
new_store (Pool *pool)
{
  Attrstore *s = calloc (1, sizeof (Attrstore));
  s->pool = pool;
  s->nameids = calloc (128, sizeof (s->nameids[0]));
  s->num_nameids = 2;
  s->nameids[0] = 0;
  s->nameids[1] = 1;

  int totalsize = strlen ("<NULL>") + 1 + 1;
  int count = 2;

  // alloc appropriate space
  s->stringspace = (char *)xmalloc((totalsize + STRINGSPACE_BLOCK) & ~STRINGSPACE_BLOCK);
  s->strings = (Offset *)xmalloc(((count + STRING_BLOCK) & ~STRING_BLOCK) * sizeof(Offset));

  // now copy predefined strings into allocated space
  s->sstrings = 0;
  strcpy (s->stringspace + s->sstrings, "<NULL>");
  s->strings[0] = s->sstrings;
  s->sstrings += strlen (s->stringspace + s->strings[0]) + 1;
  strcpy (s->stringspace + s->sstrings, "");
  s->strings[1] = s->sstrings;
  s->sstrings += strlen (s->stringspace + s->strings[1]) + 1;

  s->nstrings = 2;

  return s;
}

LocalId
str2localid (Attrstore *s, const char *str, int create)
{
  Hashval h;
  unsigned int hh;
  Hashmask hashmask;
  int i, space_needed;
  LocalId id;
  Hashtable hashtbl;

  // check string
  if (!str)
    return LOCALID_NULL;
  if (!*str)
    return LOCALID_EMPTY;

  hashmask = s->stringhashmask;
  hashtbl = s->stringhashtbl;

  // expand hashtable if needed
  if (s->nstrings * 2 > hashmask)
    {
      xfree(hashtbl);

      // realloc hash table
      s->stringhashmask = hashmask = mkmask(s->nstrings + STRING_BLOCK);
      s->stringhashtbl = hashtbl = (Hashtable)xcalloc(hashmask + 1, sizeof(Id));

      // rehash all strings into new hashtable
      for (i = 1; i < s->nstrings; i++)
	{
	  h = strhash(s->stringspace + s->strings[i]) & hashmask;
	  hh = HASHCHAIN_START;
	  while (hashtbl[h] != 0)
	    h = HASHCHAIN_NEXT(h, hh, hashmask);
	  hashtbl[h] = i;
	}
    }

  // compute hash and check for match

  h = strhash(str) & hashmask;
  hh = HASHCHAIN_START;
  while ((id = hashtbl[h]) != 0)
    {
      // break if string already hashed
      if(!strcmp(s->stringspace + s->strings[id], str))
	break;
      h = HASHCHAIN_NEXT(h, hh, hashmask);
    }
  if (id || !create)    // exit here if string found
    return id;

  // generate next id and save in table
  id = s->nstrings++;
  hashtbl[h] = id;

  if ((id & STRING_BLOCK) == 0)
    s->strings = xrealloc(s->strings, ((s->nstrings + STRING_BLOCK) & ~STRING_BLOCK) * sizeof(Hashval));
  // 'pointer' into stringspace is Offset of next free pos: sstrings
  s->strings[id] = s->sstrings;

  space_needed = strlen(str) + 1;

  // resize string buffer if needed
  if (((s->sstrings + space_needed - 1) | STRINGSPACE_BLOCK) != ((s->sstrings - 1) | STRINGSPACE_BLOCK))
    s->stringspace = xrealloc(s->stringspace, (s->sstrings + space_needed + STRINGSPACE_BLOCK) & ~STRINGSPACE_BLOCK);
  // copy new string into buffer
  memcpy(s->stringspace + s->sstrings, str, space_needed);
  // next free pos is behind new string
  s->sstrings += space_needed;

  return id;
}

const char *
localid2str(Attrstore *s, LocalId id)
{
  return s->stringspace + s->strings[id];
}

static NameId
id2nameid (Attrstore *s, Id id)
{
  unsigned int i;
  for (i = 0; i < s->num_nameids; i++)
    if (s->nameids[i] == id)
      return i;
  if (s->num_nameids >= (1 << NAME_WIDTH))
    {
      fprintf (stderr, "Too many attribute names\n");
      exit (1);
    }
  if ((s->num_nameids & 127) == 0)
    s->nameids = realloc (s->nameids, ((s->num_nameids+128) * sizeof (s->nameids[0])));
  s->nameids[s->num_nameids++] = id;
  return s->num_nameids - 1;
}

NameId
str2nameid (Attrstore *s, const char *str)
{
  return id2nameid (s, str2id (s->pool, str, 1));
}

void
ensure_entry (Attrstore *s, unsigned int entry)
{
  unsigned int old_num = s->entries;
  if (entry < s->entries)
    return;
  s->entries = entry + 1;
  if (((old_num + 127) & ~127) != ((s->entries + 127) & ~127))
    {
      if (s->attrs)
        s->attrs = realloc (s->attrs, (((s->entries+127) & ~127) * sizeof (s->attrs[0])));
      else
        s->attrs = malloc (((s->entries+127) & ~127) * sizeof (s->attrs[0]));
    }
  memset (s->attrs + old_num, 0, (s->entries - old_num) * sizeof (s->attrs[0]));
}

unsigned int
new_entry (Attrstore *s)
{
  if ((s->entries & 127) == 0)
    {
      if (s->attrs)
        s->attrs = realloc (s->attrs, ((s->entries+128) * sizeof (s->attrs[0])));
      else
        s->attrs = malloc ((s->entries+128) * sizeof (s->attrs[0]));
    }
  s->attrs[s->entries++] = 0;
  return s->entries - 1;
}

static LongNV *
find_attr (Attrstore *s, unsigned int entry, NameId name)
{
  LongNV *nv;
  if (entry >= s->entries)
    return 0;
  if (name >= s->num_nameids)
    return 0;
  nv = s->attrs[entry];
  if (nv)
    {
      while (nv->name && nv->name != name)
	nv++;
      if (nv->name)
        return nv;
    }
  return 0;
}

static void
add_attr (Attrstore *s, unsigned int entry, LongNV attr)
{
  LongNV *nv;
  unsigned int len;
  if (entry >= s->entries)
    return;
  if (attr.name >= s->num_nameids)
    return;
  nv = s->attrs[entry];
  len = 0;
  if (nv)
    {
      while (nv->name && nv->name != attr.name)
	nv++;
      if (nv->name)
        return;
      len = nv - s->attrs[entry];
    }
  len += 2;
  if (s->attrs[entry])
    s->attrs[entry] = realloc (s->attrs[entry], len * sizeof (LongNV));
  else
    s->attrs[entry] = malloc (len * sizeof (LongNV));
  nv = s->attrs[entry] + len - 2;
  *nv++ = attr;
  nv->name = 0;
}

void
add_attr_int (Attrstore *s, unsigned int entry, NameId name, unsigned int val)
{
  LongNV nv;
  nv.name = name;
  nv.type = ATTR_INT;
  nv.v.i[0] = val;
  add_attr (s, entry, nv);
}

void
add_attr_chunk (Attrstore *s, unsigned int entry, NameId name, unsigned int ofs, unsigned int len)
{
  LongNV nv;
  nv.name = name;
  nv.type = ATTR_CHUNK;
  nv.v.i[0] = ofs;
  nv.v.i[1] = len;
  add_attr (s, entry, nv);
}

void
add_attr_string (Attrstore *s, unsigned int entry, NameId name, const char *val)
{
  LongNV nv;
  nv.name = name;
  nv.type = ATTR_STRING;
  nv.v.str = strdup (val);
  add_attr (s, entry, nv);
}

void
add_attr_id (Attrstore *s, unsigned int entry, NameId name, Id val)
{
  LongNV nv;
  nv.name = name;
  nv.type = ATTR_ID;
  nv.v.i[0] = val;
  add_attr (s, entry, nv);
}

void
add_attr_intlist_int (Attrstore *s, unsigned int entry, NameId name, int val)
{
  LongNV *nv = find_attr (s, entry, name);
  if (val == 0)
    return;
  if (nv)
    {
      unsigned len = 0;
      while (nv->v.intlist[len])
        len++;
      nv->v.intlist = realloc (nv->v.intlist, (len + 2) * sizeof (nv->v.intlist[0]));
      nv->v.intlist[len] = val;
      nv->v.intlist[len+1] = 0;
    }
  else
    {
      LongNV mynv;
      mynv.name = name;
      mynv.type = ATTR_INTLIST;
      mynv.v.intlist = malloc (2 * sizeof (mynv.v.intlist[0]));
      mynv.v.intlist[0] = val;
      mynv.v.intlist[1] = 0;
      add_attr (s, entry, mynv);
    }
}

void
add_attr_localids_id (Attrstore *s, unsigned int entry, NameId name, LocalId id)
{
  LongNV *nv = find_attr (s, entry, name);
  if (nv)
    {
      unsigned len = 0;
      while (nv->v.localids[len])
        len++;
      nv->v.localids = realloc (nv->v.localids, (len + 2) * sizeof (nv->v.localids[0]));
      nv->v.localids[len] = id;
      nv->v.localids[len+1] = 0;
    }
  else
    {
      LongNV mynv;
      mynv.name = name;
      mynv.type = ATTR_LOCALIDS;
      mynv.v.localids = malloc (2 * sizeof (mynv.v.localids[0]));
      mynv.v.localids[0] = id;
      mynv.v.localids[1] = 0;
      add_attr (s, entry, mynv);
    }
}

#define FLAT_ATTR_BLOCK 127
#define ABBR_BLOCK 127
#define FLAT_ABBR_BLOCK 127
#define add_elem(buf,ofs,val,block) do { \
  if (((ofs) & (block)) == 0) \
    buf = xrealloc (buf, ((ofs) + (block) + 1) * sizeof((buf)[0])); \
  (buf)[(ofs)++] = val; \
} while (0)
#define add_u16(buf,ofs,val,block) do {\
  typedef int __wrong_buf__[(1-sizeof((buf)[0])) * (sizeof((buf)[0])-1)];\
  add_elem(buf,ofs,(val) & 0xFF,block); \
  add_elem(buf,ofs,((val) >> 8) & 0xFF,block); \
} while (0)
#define add_num(buf,ofs,val,block) do {\
  typedef int __wrong_buf__[(1-sizeof((buf)[0])) * (sizeof((buf)[0])-1)];\
  if ((val) >= (1 << 14)) \
    { \
      if ((val) >= (1 << 28)) \
	add_elem (buf,ofs,((val) >> 28) | 128, block); \
      if ((val) >= (1 << 21)) \
	add_elem (buf,ofs,((val) >> 21) | 128, block); \
      add_elem (buf,ofs,((val) >> 14) | 128, block); \
    } \
  if ((val) >= (1 << 7)) \
    add_elem (buf,ofs,((val) >> 7) | 128, block); \
  add_elem (buf,ofs,(val) & 127, block); \
} while (0)

static int
longnv_cmp (const void *pa, const void *pb)
{
  const LongNV *a = (const LongNV *)pa;
  const LongNV *b = (const LongNV *)pb;
  int r = a->name - b->name;
  if (r)
    return r;
  r = a->type - b->type;
  return r;
}

void
attr_store_pack (Attrstore *s)
{
  unsigned i;
  unsigned int old_mem = 0;
  if (s->packed)
    return;
  s->ent2attr = xcalloc (s->entries, sizeof (s->ent2attr[0]));
  s->flat_attrs = 0;
  s->attr_next_free = 0;
  s->abbr = 0;
  s->abbr_next_free = 0;
  s->flat_abbr = 0;
  s->flat_abbr_next_free = 0;

  add_num (s->flat_attrs, s->attr_next_free, 0, FLAT_ATTR_BLOCK);
  add_elem (s->abbr, s->abbr_next_free, 0, ABBR_BLOCK);
  add_elem (s->flat_abbr, s->flat_abbr_next_free, 0, FLAT_ABBR_BLOCK);

  for (i = 0; i < s->entries; i++)
    {
      unsigned int num_attrs = 0, ofs;
      LongNV *nv = s->attrs[i];
      if (nv)
        while (nv->name)
	  nv++, num_attrs++;
      if (nv)
        old_mem += (num_attrs + 1) * sizeof (LongNV);
      if (!num_attrs)
        continue;
      qsort (s->attrs[i], num_attrs, sizeof (LongNV), longnv_cmp);
      unsigned int this_abbr;
      nv = s->attrs[i];
      for (this_abbr = 0; this_abbr < s->abbr_next_free; this_abbr++)
        {
	  for (ofs = 0; ofs < num_attrs; ofs++)
	    {
	      unsigned short name_type = (nv[ofs].name << 4) | nv[ofs].type;
	      assert (s->abbr[this_abbr] + ofs < s->flat_abbr_next_free);
	      if (name_type != s->flat_abbr[s->abbr[this_abbr]+ofs])
		break;
	    }
	  if (ofs == num_attrs && !s->flat_abbr[s->abbr[this_abbr]+ofs])
	    break;
	}
      if (this_abbr == s->abbr_next_free)
        {
	  /* This schema not found --> insert it.  */
	  add_elem (s->abbr, s->abbr_next_free, s->flat_abbr_next_free, ABBR_BLOCK);
	  for (ofs = 0; ofs < num_attrs; ofs++)
	    {
	      unsigned short name_type = (nv[ofs].name << 4) | nv[ofs].type;
	      add_elem (s->flat_abbr, s->flat_abbr_next_free, name_type, FLAT_ABBR_BLOCK);
	    }
	  add_elem (s->flat_abbr, s->flat_abbr_next_free, 0, FLAT_ABBR_BLOCK);
	}
      s->ent2attr[i] = s->attr_next_free;
      add_num (s->flat_attrs, s->attr_next_free, this_abbr, FLAT_ATTR_BLOCK);
      for (ofs = 0; ofs < num_attrs; ofs++)
	switch (nv[ofs].type)
	  {
	    case ATTR_INT:
	    case ATTR_ID:
	      {
	        unsigned int i = nv[ofs].v.i[0];
		add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
	        break;
	      }
	    case ATTR_CHUNK:
	      {
	        unsigned int i = nv[ofs].v.i[0];
		add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
		i = nv[ofs].v.i[1];
		add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
	        break;
	      }
	    case ATTR_STRING:
	      {
	        const char *str = nv[ofs].v.str;
		for (; *str; str++)
		  add_elem (s->flat_attrs, s->attr_next_free, *str, FLAT_ATTR_BLOCK);
		add_elem (s->flat_attrs, s->attr_next_free, 0, FLAT_ATTR_BLOCK);
		old_mem += strlen ((const char*)nv[ofs].v.str) + 1;
		xfree ((void*)nv[ofs].v.str);
		break;
	      }
	    case ATTR_INTLIST:
	      {
		const int *il = nv[ofs].v.intlist;
		int i;
		for (; (i = *il) != 0; il++, old_mem += 4)
		  add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
		add_num (s->flat_attrs, s->attr_next_free, 0, FLAT_ATTR_BLOCK);
		old_mem+=4;
		xfree (nv[ofs].v.intlist);
	        break;
	      }
	    case ATTR_LOCALIDS:
	      {
		const Id *il = nv[ofs].v.localids;
		Id i;
		for (; (i = *il) != 0; il++, old_mem += 4)
		  add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
		add_num (s->flat_attrs, s->attr_next_free, 0, FLAT_ATTR_BLOCK);
		old_mem += 4;
		xfree (nv[ofs].v.localids);
	        break;
	      }
	    default:
	      break;
	  }
      xfree (nv);
    }
  old_mem += s->entries * sizeof (s->attrs[0]);
  free (s->attrs);
  s->attrs = 0;

  /* Remove the hashtable too, it will be build on demand in str2localid
     the next time we call it, which should not happen while in packed mode.  */
  old_mem += (s->stringhashmask + 1) * sizeof (s->stringhashtbl[0]);
  free (s->stringhashtbl);
  s->stringhashtbl = 0;
  s->stringhashmask = 0;

  fprintf (stderr, "%d\n", old_mem);
  fprintf (stderr, "%d\n", s->entries * sizeof(s->ent2attr[0]));
  fprintf (stderr, "%d\n", s->attr_next_free);
  fprintf (stderr, "%d\n", s->abbr_next_free * sizeof(s->abbr[0]));
  fprintf (stderr, "%d\n", s->flat_abbr_next_free * sizeof(s->flat_abbr[0]));
  s->packed = 1;
}


void
attr_store_unpack (Attrstore *s)
{
  unsigned int i;
  if (!s->packed)
    return;

  /* Make the store writable right away, so we can use our adder functions.  */
  s->packed = 0;
  s->attrs = xcalloc (s->entries, sizeof (s->attrs[0]));

  for (i = 0; i < s->entries; i++)
    {
      attr_iterator ai;
      FOR_ATTRS (s, i, &ai)
        {
	  switch (ai.type)
	    {
	    case ATTR_INT:
	      add_attr_int (s, i, ai.name, ai.as_int); 
	      break;
	    case ATTR_ID:
	      add_attr_id (s, i, ai.name, ai.as_id); 
	      break;
	    case ATTR_CHUNK:
	      add_attr_chunk (s, i, ai.name, ai.as_chunk[0], ai.as_chunk[1]);
	      break;
	    case ATTR_STRING:
	      add_attr_string (s, i, ai.name, ai.as_string);
	      break;
	    case ATTR_INTLIST:
	      {
		while (1)
		  {
		    int val;
		    get_num (ai.as_numlist, val);
		    if (!val)
		      break;
		    add_attr_intlist_int (s, i, ai.name, val);
		  }
	        break;
	      }
	    case ATTR_LOCALIDS:
	      {
		while (1)
		  {
		    Id val;
		    get_num (ai.as_numlist, val);
		    if (!val)
		      break;
		    add_attr_localids_id (s, i, ai.name, val);
		  }
	        break;
	      }
	    default:
	      break;
	    }
	}
    }

  xfree (s->ent2attr);
  s->ent2attr = 0;
  xfree (s->flat_attrs);
  s->flat_attrs = 0;
  s->attr_next_free = 0;
  xfree (s->abbr);
  s->abbr = 0;
  s->abbr_next_free = 0;
  xfree (s->flat_abbr);
  s->flat_abbr = 0;
  s->flat_abbr_next_free = 0;
}

static void
write_u32(FILE *fp, unsigned int x)
{
  if (putc(x >> 24, fp) == EOF ||
      putc(x >> 16, fp) == EOF ||
      putc(x >> 8, fp) == EOF ||
      putc(x, fp) == EOF)
    {
      perror("write error");
      exit(1);
    }
}

static void
write_id(FILE *fp, Id x)
{
  if (x >= (1 << 14))
    {
      if (x >= (1 << 28))
	putc((x >> 28) | 128, fp);
      if (x >= (1 << 21))
	putc((x >> 21) | 128, fp);
      putc((x >> 14) | 128, fp);
    }
  if (x >= (1 << 7))
    putc((x >> 7) | 128, fp);
  if (putc(x & 127, fp) == EOF)
    {
      perror("write error");
      exit(1);
    }
}

void
write_attr_store (FILE *fp, Attrstore *s)
{
  unsigned i;
  unsigned local_ssize;

  attr_store_pack (s);

  write_u32 (fp, s->entries);
  write_u32 (fp, s->num_nameids);
  write_u32 (fp, s->nstrings);
  for (i = 2; i < s->num_nameids; i++)
    {
      const char *str = id2str (s->pool, s->nameids[i]);
      if (fwrite(str, strlen(str) + 1, 1, fp) != 1)
	{
	  perror("write error");
	  exit(1);
	}
    }

  for (i = 2, local_ssize = 0; i < (unsigned)s->nstrings; i++)
    local_ssize += strlen (localid2str (s, i)) + 1;

  write_u32 (fp, local_ssize);
  for (i = 2; i < (unsigned)s->nstrings; i++)
    {
      const char *str = localid2str (s, i);
      if (fwrite(str, strlen(str) + 1, 1, fp) != 1)
	{
	  perror("write error");
	  exit(1);
	}
    }

  int last = 0;
  for (i = 0; i < s->entries; i++)
    if (i == 0 || s->ent2attr[i] == 0)
      write_id (fp, s->ent2attr[i]);
    else
      {
        write_id (fp, s->ent2attr[i] - last);
	assert (last < s->ent2attr[i]);
	last = s->ent2attr[i];
      }

  write_u32 (fp, s->attr_next_free);
  if (fwrite (s->flat_attrs, s->attr_next_free, 1, fp) != 1)
    {
      perror ("write error");
      exit (1);
    }

  write_u32 (fp, s->flat_abbr_next_free);
  if (fwrite (s->flat_abbr, s->flat_abbr_next_free * sizeof (s->flat_abbr[0]), 1, fp) != 1)
    {
      perror ("write error");
      exit (1);
    }
}

static unsigned int
read_u32(FILE *fp)
{
  int c, i;
  unsigned int x = 0;

  for (i = 0; i < 4; i++)
    {
      c = getc(fp);
      if (c == EOF)
	{
	  fprintf(stderr, "unexpected EOF\n");
	  exit(1);
	}
      x = (x << 8) | c;
    }
  return x;
}

static unsigned int
read_u8(FILE *fp)
{
  int c;
  c = getc(fp);
  if (c == EOF)
    {
      fprintf(stderr, "unexpected EOF\n");
      exit(1);
    }
  return c;
}

static Id
read_id(FILE *fp, Id max)
{
  unsigned int x = 0;
  int c, i;

  for (i = 0; i < 5; i++)
    {
      c = getc(fp);
      if (c == EOF)
	{
	  fprintf(stderr, "unexpected EOF\n");
	  exit(1);
	}
      if (!(c & 128))
	{
	  x = (x << 7) | c;
	  if (max && x >= max)
	    {
	      fprintf(stderr, "read_id: id too large (%u/%u)\n", x, max);
	      exit(1);
	    }
	  return x;
	}
      x = (x << 7) ^ c ^ 128;
    }
  fprintf(stderr, "read_id: id too long\n");
  exit(1);
}

Attrstore *
attr_store_read (FILE *fp, Pool *pool)
{
  unsigned nentries;
  unsigned i;
  unsigned local_ssize;
  unsigned nstrings;
  char *buf;
  size_t buflen;
  Attrstore *s = new_store (pool);

  nentries = read_u32 (fp);
  s->num_nameids = read_u32 (fp);
  nstrings = read_u32 (fp);

  buflen = 128;
  buf = malloc (buflen);

  s->nameids = realloc (s->nameids, (((s->num_nameids+127) & ~127) * sizeof (s->nameids[0])));
  for (i = 2; i < s->num_nameids; i++)
    {
      size_t p = 0;
      while (1)
        {
	  int c = read_u8 (fp);
	  if (p == buflen)
	    {
	      buflen += 128;
	      buf = realloc (buf, buflen);
	    }
	  buf[p++] = c;
	  if (!c)
	    break;
	}
      s->nameids[i] = str2id (s->pool, buf, 1);
    }

  local_ssize = read_u32 (fp);
  char *strsp = (char *)xrealloc(s->stringspace, s->sstrings + local_ssize + 1);
  Offset *str = (Offset *)xrealloc(s->strings, (nstrings) * sizeof(Offset));

  s->stringspace = strsp;
  s->strings = str;
  strsp += s->sstrings;

  if (fread(strsp, local_ssize, 1, fp) != 1)
    {
      perror ("read error while reading strings");
      exit(1);
    }
  strsp[local_ssize] = 0;

  /* Don't build hashtable here, it will be built on demand by str2localid
     should we call that.  */

  strsp = s->stringspace;
  s->nstrings = nstrings;
  for (i = 0; i < nstrings; i++)
    {
      str[i] = strsp - s->stringspace;
      strsp += strlen (strsp) + 1;
    }
  s->sstrings = strsp - s->stringspace;

  s->entries = nentries;

  s->ent2attr = xmalloc (s->entries * sizeof (s->ent2attr[0]));
  int last = 0;
  for (i = 0; i < s->entries; i++)
    {
      int d = read_id (fp, 0);
      if (i == 0 || d == 0)
        s->ent2attr[i] = d;
      else
        {
	  last += d;
	  s->ent2attr[i] = last;
	}
    }

  s->attr_next_free = read_u32 (fp);
  s->flat_attrs = xmalloc (((s->attr_next_free + FLAT_ATTR_BLOCK) & ~(FLAT_ATTR_BLOCK + 1)) * sizeof (s->flat_attrs[0]));
  if (fread (s->flat_attrs, s->attr_next_free, 1, fp) != 1)
    {
      perror ("read error");
      exit (1);
    }

  s->flat_abbr_next_free = read_u32 (fp);
  s->flat_abbr = xmalloc (((s->flat_abbr_next_free + FLAT_ABBR_BLOCK) & ~(FLAT_ABBR_BLOCK + 1)) * sizeof (s->flat_abbr[0]));
  if (fread (s->flat_abbr, s->flat_abbr_next_free * sizeof (s->flat_abbr[0]), 1, fp) != 1)
    {
      perror ("read error");
      exit (1);
    }

  assert (s->flat_abbr[0] == 0);
  s->abbr_next_free = 0;
  s->abbr = 0;
  add_elem (s->abbr, s->abbr_next_free, 0, ABBR_BLOCK);

  unsigned int abbi;
  for (abbi = 0; abbi < s->flat_abbr_next_free - 1; abbi++)
    if (s->flat_abbr[abbi] == 0)
      add_elem (s->abbr, s->abbr_next_free, abbi + 1, ABBR_BLOCK);
  assert (s->flat_abbr[abbi] == 0);

  s->packed = 1;

  free (buf);

  return s;
}

#ifdef MAIN
int
main (void)
{
  Pool *pool = pool_create ();
  Attrstore *s = new_store (pool);
  unsigned int id1 = new_entry (s);
  unsigned int id2 = new_entry (s);
  unsigned int id3 = new_entry (s);
  unsigned int id4 = new_entry (s);
  add_attr_int (s, id1, str2nameid (s, "name1"), 42);
  add_attr_chunk (s, id1, str2nameid (s, "name2"), 9876, 1024);
  add_attr_string (s, id1, str2nameid (s, "name3"), "hallo");
  add_attr_int (s, id1, str2nameid (s, "name1"), 43);
  add_attr_id (s, id1, str2nameid (s, "id1"), 100);
  add_attr_intlist_int (s, id1, str2nameid (s, "intlist1"), 3);
  add_attr_intlist_int (s, id1, str2nameid (s, "intlist1"), 14);
  add_attr_intlist_int (s, id1, str2nameid (s, "intlist1"), 1);
  add_attr_intlist_int (s, id1, str2nameid (s, "intlist1"), 59);
  add_attr_localids_id (s, id1, str2nameid (s, "l_ids1"), str2localid (s, "one", 1));
  add_attr_localids_id (s, id1, str2nameid (s, "l_ids1"), str2localid (s, "two", 1));
  add_attr_localids_id (s, id1, str2nameid (s, "l_ids1"), str2localid (s, "three", 1));
  add_attr_localids_id (s, id1, str2nameid (s, "l_ids2"), str2localid (s, "three", 1));
  add_attr_localids_id (s, id1, str2nameid (s, "l_ids2"), str2localid (s, "two", 1));
  add_attr_localids_id (s, id1, str2nameid (s, "l_ids2"), str2localid (s, "one", 1));
  write_attr_store (stdout, s);
  return 0;
}
#endif
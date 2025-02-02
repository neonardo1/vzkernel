#ifndef _UAPI_FALLOC_H_
#define _UAPI_FALLOC_H_

#define FALLOC_FL_KEEP_SIZE	0x01 /* default is extend size */
#define FALLOC_FL_PUNCH_HOLE	0x02 /* de-allocates range */
#define FALLOC_FL_NO_HIDE_STALE	0x04 /* reserved codepoint */
#define FALLOC_FL_CONVERT_AND_EXTEND 0x100 /* mark extents as initialized
					    * and extend i_size */



#endif /* _UAPI_FALLOC_H_ */

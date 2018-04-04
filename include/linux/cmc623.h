#ifndef __CMC623_H__
#define __CMC623_H__

#ifdef CONFIG_FB_TEGRA_CMC623

extern void cmc623_suspend(void);
extern void cmc623_resume(void);

#else

inline void cmc623_suspend(void) { }
inline void cmc623_resume(void) { }

#endif /* CONFIG_FB_TEGRA_CMC623 */
#endif  /* __CMC623_H__ */

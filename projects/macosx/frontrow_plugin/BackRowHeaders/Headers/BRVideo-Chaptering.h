/*
 *     Generated by class-dump 3.1.1.
 *
 *     class-dump is Copyright (C) 1997-1998, 2000-2001, 2004-2006 by Steve Nygard.
 */

#import <BackRow/BRVideo.h>

@interface BRVideo (Chaptering)
- (void)_updateChapters;
- (void)_generateFakeChapters;
- (double)_keyframeNearChapter:(double)fp8 threshold:(float)fp16;
- (long)_searchForChapterAtTime:(double)fp8;
- (void)_disableDevilTracks;
@end


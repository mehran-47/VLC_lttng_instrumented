/*
 *     Generated by class-dump 3.1.1.
 *
 *     class-dump is Copyright (C) 1997-1998, 2000-2001, 2004-2006 by Steve Nygard.
 */

#import <BackRow/BRMediaMenuController.h>

@class NSArray, NSMutableDictionary, NSPredicate, NSString;

@interface BRMusicKeyFilterTracklistMenuController : BRMediaMenuController
{
    id <BRMusicKeyFilterSelectionTarget> _selectionDelegate;
    NSString *_key;
    NSString *_sortKey;
    NSArray *_tracks;
    long _numTracks;
    NSArray *_values;
    long _numValues;
    unsigned int _allowAllTracks:1;
    unsigned int _allowShuffle:1;
    unsigned int _showMetadata:1;
    NSPredicate *_predicate;
    NSMutableDictionary *_menuItemCollections;
}

+ (id)menuControllerWithPredicate:(id)fp8 keyFilter:(id)fp12 sortKey:(id)fp16;
- (id)initWithPredicate:(id)fp8 keyFilter:(id)fp12 sortKey:(id)fp16;
- (void)dealloc;
- (BOOL)isVolatile;
- (id)loadModelData;
- (void)refreshControllerForModelUpdate;
- (BOOL)shouldRefreshForUpdateToObject:(id)fp8;
- (void)setSelectionDelegate:(id)fp8;
- (void)setHasShuffleOption:(BOOL)fp8;
- (void)setShowMetadataOption:(BOOL)fp8;
- (void)itemSelected:(long)fp8;
- (id)previewControlForItem:(long)fp8;
- (BOOL)mediaPreviewShouldShowMetadata;
- (id)mediaPreviewMissingMediaType;

@end

/**********************************************************************

  Audacity: A Digital Audio Editor

  Track.cpp

  Dominic Mazzoni

*******************************************************************//**

\class Track
\brief Fundamental data object of Audacity, placed in the TrackPanel.
Classes derived form it include the WaveTrack, NoteTrack, LabelTrack
and TimeTrack.

\class AudioTrack
\brief A Track that can load/save audio data to/from XML.

\class PlayableTrack
\brief An AudioTrack that can be played and stopped.

*//*******************************************************************/

#include <algorithm>
#include <numeric>
#include "Track.h"

#include <float.h>
#include <wx/file.h>
#include <wx/textfile.h>
#include <wx/log.h>

#include "TimeTrack.h"
#include "WaveTrack.h"
#include "NoteTrack.h"
#include "LabelTrack.h"
#include "Project.h"
#include "DirManager.h"

#include "Experimental.h"

#include "TrackPanel.h" // for TrackInfo

#ifdef _MSC_VER
//Disable truncation warnings
#pragma warning( disable : 4786 )
#endif

Track::Track(const std::shared_ptr<DirManager> &projDirManager)
:  vrulerSize(36,0),
   mDirManager(projDirManager)
{
   mSelected  = false;
   mLinked    = false;

   mY = 0;
   mHeight = DefaultHeight;
   mIndex = 0;

   mMinimized = false;

   mOffset = 0.0;

   mChannel = MonoChannel;
}

Track::Track(const Track &orig)
: vrulerSize( orig.vrulerSize )
{
   mY = 0;
   mIndex = 0;
   Init(orig);
   mOffset = orig.mOffset;
}

// Copy all the track properties except the actual contents
void Track::Init(const Track &orig)
{
   mId = orig.mId;

   mDefaultName = orig.mDefaultName;
   mName = orig.mName;

   mDirManager = orig.mDirManager;

   mSelected = orig.mSelected;
   mLinked = orig.mLinked;
   mHeight = orig.mHeight;
   mMinimized = orig.mMinimized;
   mChannel = orig.mChannel;
}

void Track::SetSelected(bool s)
{
   mSelected = s;
}

void Track::Merge(const Track &orig)
{
   mSelected = orig.mSelected;
}

Track::~Track()
{
}


TrackNodePointer Track::GetNode() const
{
   wxASSERT(mList.lock() == NULL || this == mNode.first->get());
   return mNode;
}

void Track::SetOwner
(const std::weak_ptr<TrackList> &list, TrackNodePointer node)
{
   // BUG: When using this function to clear an owner, we may need to clear 
   // focussed track too.  Otherwise focus could remain on an invisible (or deleted) track.
   mList = list;
   mNode = node;
}

int Track::GetMinimizedHeight() const
{
   auto height = TrackInfo::MinimumTrackHeight();
   auto channels = TrackList::Channels(this);
   auto nChannels = channels.size();
   auto begin = channels.begin();
   auto index = std::distance(begin, std::find(begin, channels.end(), this));
   return (height * (index + 1) / nChannels) - (height * index / nChannels);
}

int Track::GetIndex() const
{
   return mIndex;
}

void Track::SetIndex(int index)
{
   mIndex = index;
}

int Track::GetY() const
{
   return mY;
}

void Track::SetY(int y)
{
   auto pList = mList.lock();
   if (pList && !pList->mPendingUpdates.empty()) {
      auto orig = pList->FindById( GetId() );
      if (orig && orig != this) {
         // delegate, and rely on the update to copy back
         orig->SetY(y);
         pList->UpdatePendingTracks();
         return;
      }
   }

   DoSetY(y);
}

void Track::DoSetY(int y)
{
   mY = y;
}

int Track::GetHeight() const
{
   if (mMinimized) {
      return GetMinimizedHeight();
   }

   return mHeight;
}

void Track::SetHeight(int h)
{
   auto pList = mList.lock();
   if (pList && !pList->mPendingUpdates.empty()) {
      auto orig = pList->FindById( GetId() );
      if (orig && orig != this) {
         // delegate, and rely on RecalcPositions to copy back
         orig->SetHeight(h);
         return;
      }
   }

   DoSetHeight(h);

   if (pList) {
      pList->RecalcPositions(mNode);
      pList->ResizingEvent(mNode);
   }
}

void Track::DoSetHeight(int h)
{
   mHeight = h;
}

bool Track::GetMinimized() const
{
   return mMinimized;
}

void Track::SetMinimized(bool isMinimized)
{
   auto pList = mList.lock();
   if (pList && !pList->mPendingUpdates.empty()) {
      auto orig = pList->FindById( GetId() );
      if (orig && orig != this) {
         // delegate, and rely on RecalcPositions to copy back
         orig->SetMinimized(isMinimized);
         return;
      }
   }

   DoSetMinimized(isMinimized);

   if (pList) {
      pList->RecalcPositions(mNode);
      pList->ResizingEvent(mNode);
   }
}

void Track::DoSetMinimized(bool isMinimized)
{
   mMinimized = isMinimized;
}

void Track::SetLinked(bool l)
{
   auto pList = mList.lock();
   if (pList && !pList->mPendingUpdates.empty()) {
      auto orig = pList->FindById( GetId() );
      if (orig && orig != this) {
         // delegate, and rely on RecalcPositions to copy back
         orig->SetLinked(l);
         return;
      }
   }

   DoSetLinked(l);

   if (pList) {
      pList->RecalcPositions(mNode);
      pList->ResizingEvent(mNode);
   }
}

void Track::DoSetLinked(bool l)
{
   mLinked = l;
}

Track *Track::GetLink() const
{
   auto pList = mList.lock();
   if (!pList)
      return nullptr;

   if (!pList->isNull(mNode)) {
      if (mLinked) {
         auto next = pList->getNext( mNode );
         if ( !pList->isNull( next ) )
            return next.first->get();
      }

      if (mNode.first != mNode.second->begin()) {
         auto prev = pList->getPrev( mNode );
         if ( !pList->isNull( prev ) ) {
            auto track = prev.first->get();
            if (track && track->GetLinked())
               return track;
         }
      }
   }

   return nullptr;
}

namespace {
   inline bool IsSyncLockableNonLabelTrack( const Track *pTrack )
   {
      return nullptr != track_cast< const AudioTrack * >( pTrack );
   }

   bool IsGoodNextSyncLockTrack(const Track *t, bool inLabelSection)
   {
      if (!t)
         return false;
      const bool isLabel = ( nullptr != track_cast<const LabelTrack*>(t) );
      if (inLabelSection)
         return isLabel;
      else if (isLabel)
         return true;
      else
         return IsSyncLockableNonLabelTrack( t );
   }
}

bool Track::IsSyncLockSelected() const
{
#ifdef EXPERIMENTAL_SYNC_LOCK
   AudacityProject *p = GetActiveProject();
   if (!p || !p->IsSyncLocked())
      return false;

   auto pList = mList.lock();
   if (!pList)
      return false;
   auto trackRange = TrackList::SyncLockGroup(this);

   if (trackRange.size() <= 1) {
      // Not in a sync-locked group.
      // Return true iff selected and of a sync-lockable type.
      return (IsSyncLockableNonLabelTrack(this) ||
              track_cast<const LabelTrack*>(this)) && GetSelected();
   }

   // Return true iff any track in the group is selected.
   return *(trackRange + &Track::IsSelected).begin();
#endif

   return false;
}

void Track::SyncLockAdjust(double oldT1, double newT1)
{
   if (newT1 > oldT1) {
      // Insert space within the track

      if (oldT1 > GetEndTime())
         return;

      auto tmp = Cut(oldT1, GetEndTime());

      Paste(newT1, tmp.get());
   }
   else if (newT1 < oldT1) {
      // Remove from the track
      Clear(newT1, oldT1);
   }
}

std::shared_ptr<Track> Track::FindTrack()
{
   return Pointer( this );
}

void PlayableTrack::Init( const PlayableTrack &orig )
{
   mMute = orig.mMute;
   mSolo = orig.mSolo;
   AudioTrack::Init( orig );
}

void PlayableTrack::Merge( const Track &orig )
{
   auto pOrig = dynamic_cast<const PlayableTrack *>(&orig);
   wxASSERT( pOrig );
   mMute = pOrig->mMute;
   mSolo = pOrig->mSolo;
   AudioTrack::Merge( *pOrig );
}

// Serialize, not with tags of its own, but as attributes within a tag.
void PlayableTrack::WriteXMLAttributes(XMLWriter &xmlFile) const
{
   xmlFile.WriteAttr(wxT("mute"), mMute);
   xmlFile.WriteAttr(wxT("solo"), mSolo);
   AudioTrack::WriteXMLAttributes(xmlFile);
}

// Return true iff the attribute is recognized.
bool PlayableTrack::HandleXMLAttribute(const wxChar *attr, const wxChar *value)
{
   const wxString strValue{ value };
   long nValue;
   if (!wxStrcmp(attr, wxT("mute")) &&
            XMLValueChecker::IsGoodInt(strValue) && strValue.ToLong(&nValue)) {
      mMute = (nValue != 0);
      return true;
   }
   else if (!wxStrcmp(attr, wxT("solo")) &&
            XMLValueChecker::IsGoodInt(strValue) && strValue.ToLong(&nValue)) {
      mSolo = (nValue != 0);
      return true;
   }

   return AudioTrack::HandleXMLAttribute(attr, value);
}

bool Track::Any() const
   { return true; }

bool Track::IsSelected() const
   { return GetSelected(); }

bool Track::IsSelectedOrSyncLockSelected() const
   { return GetSelected() || IsSyncLockSelected(); }

bool Track::IsLeader() const
   { return !GetLink() || GetLinked(); }

bool Track::IsSelectedLeader() const
   { return IsSelected() && IsLeader(); }

std::pair<Track *, Track *> TrackList::FindSyncLockGroup(Track *pMember) const
{
   if (!pMember)
      return { nullptr, nullptr };

   // A non-trivial sync-locked group is a maximal sub-sequence of the tracks
   // consisting of any positive number of audio tracks followed by zero or
   // more label tracks.

   // Step back through any label tracks.
   auto member = pMember;
   while (member && ( nullptr != track_cast<const LabelTrack*>(member) )) {
      member = GetPrev(member);
   }

   // Step back through the wave and note tracks before the label tracks.
   Track *first = nullptr;
   while (member && IsSyncLockableNonLabelTrack(member)) {
      first = member;
      member = GetPrev(member);
   }

   if (!first)
      // Can't meet the criteria described above.  In that case,
      // consider the track to be the sole member of a group.
      return { pMember, pMember };

   Track *last = first;
   bool inLabels = false;

   while (const auto next = GetNext(last)) {
      if ( ! IsGoodNextSyncLockTrack(next, inLabels) )
         break;
      last = next;
      inLabels = (nullptr != track_cast<const LabelTrack*>(last) );
   }

   return { first, last };
}


// TrackList
//
// The TrackList sends events whenever certain updates occur to the list it
// is managing.  Any other classes that may be interested in get these updates
// should use TrackList::Connect() or TrackList::Bind().
//
wxDEFINE_EVENT(EVT_TRACKLIST_PERMUTED, wxCommandEvent);
wxDEFINE_EVENT(EVT_TRACKLIST_RESIZING, wxCommandEvent);
wxDEFINE_EVENT(EVT_TRACKLIST_DELETION, wxCommandEvent);

// same value as in the default constructed TrackId:
long TrackList::sCounter = -1;

TrackList::TrackList()
:  wxEvtHandler()
{
}

// Factory function
std::shared_ptr<TrackList> TrackList::Create()
{
   std::shared_ptr<TrackList> result{ safenew TrackList{} };
   result->mSelf = result;
   return result;
}

TrackList &TrackList::operator= (TrackList &&that)
{
   if (this != &that) {
      this->Clear();
      Swap(that);
   }
   return *this;
}

void TrackList::Swap(TrackList &that)
{
   auto SwapLOTs = [](
      ListOfTracks &a, const std::weak_ptr< TrackList > &aSelf,
      ListOfTracks &b, const std::weak_ptr< TrackList > &bSelf )
   {
      a.swap(b);
      for (auto it = a.begin(), last = a.end(); it != last; ++it)
         (*it)->SetOwner(aSelf, {it, &a});
      for (auto it = b.begin(), last = b.end(); it != last; ++it)
         (*it)->SetOwner(bSelf, {it, &b});
   };

   SwapLOTs( *this, mSelf, that, that.mSelf );
   SwapLOTs( this->mPendingUpdates, mSelf, that.mPendingUpdates, that.mSelf );
   mUpdaters.swap(that.mUpdaters);
}

TrackList::~TrackList()
{
   Clear(false);
}

void TrackList::RecalcPositions(TrackNodePointer node)
{
   if ( isNull( node ) )
      return;

   Track *t;
   int i = 0;
   int y = 0;

   auto prev = getPrev( node );
   if ( !isNull( prev ) ) {
      t = prev.first->get();
      i = t->GetIndex() + 1;
      y = t->GetY() + t->GetHeight();
   }

   const auto theEnd = end();
   for (auto n = Find( node.first->get() ); n != theEnd; ++n) {
      t = *n;
      t->SetIndex(i++);
      t->DoSetY(y);
      y += t->GetHeight();
   }

   UpdatePendingTracks();
}

void TrackList::PermutationEvent()
{
   auto e = std::make_unique<wxCommandEvent>(EVT_TRACKLIST_PERMUTED);
   // wxWidgets will own the event object
   QueueEvent(e.release());
}

void TrackList::DeletionEvent()
{
   auto e = std::make_unique<wxCommandEvent>(EVT_TRACKLIST_DELETION);
   // wxWidgets will own the event object
   QueueEvent(e.release());
}

void TrackList::ResizingEvent(TrackNodePointer node)
{
   auto e = std::make_unique<TrackListEvent>(EVT_TRACKLIST_RESIZING);
   e->mpTrack = *node.first;
   // wxWidgets will own the event object
   QueueEvent(e.release());
}

auto TrackList::EmptyRange() const
   -> TrackIterRange< Track >
{
   auto it = const_cast<TrackList*>(this)->getEnd();
   return {
      { it, it, it, &Track::Any },
      { it, it, it, &Track::Any }
   };
}

auto TrackList::SyncLockGroup( Track *pTrack )
   -> TrackIterRange< Track >
{
   auto pList = pTrack->GetOwner();
   auto tracks =
      pList->FindSyncLockGroup( const_cast<Track*>( pTrack ) );
   return pList->Any().StartingWith(tracks.first).EndingAfter(tracks.second);
}

auto TrackList::FindLeader( Track *pTrack )
   -> TrackIter< Track >
{
   auto iter = Find(pTrack);
   while( *iter && ! ( *iter )->IsLeader() )
      --iter;
   return iter.Filter( &Track::IsLeader );
}

void TrackList::Permute(const std::vector<TrackNodePointer> &permutation)
{
   for (const auto iter : permutation) {
      ListOfTracks::value_type track = std::move(*iter.first);
      erase(iter.first);
      Track *pTrack = track.get();
      pTrack->SetOwner(mSelf,
                       { insert(ListOfTracks::end(), std::move(track)), this });
   }
   auto n = getBegin();
   RecalcPositions(n);
   PermutationEvent();
}

Track *TrackList::FindById( TrackId id )
{
   // Linear search.  Tracks in a project are usually very few.
   // Search only the non-pending tracks.
   auto it = std::find_if( ListOfTracks::begin(), ListOfTracks::end(),
      [=](const ListOfTracks::value_type &ptr){ return ptr->GetId() == id; } );
   if (it == ListOfTracks::end())
      return {};
   return it->get();
}

template<typename TrackKind>
Track *TrackList::Add(std::unique_ptr<TrackKind> &&t)
{
   Track *pTrack;
   push_back(ListOfTracks::value_type(pTrack = t.release()));

   auto n = getPrev( getEnd() );

   pTrack->SetOwner(mSelf, n);
   pTrack->SetId( TrackId{ ++sCounter } );
   RecalcPositions(n);
   ResizingEvent(n);
   return back().get();
}

// Make instantiations for the linker to find
template Track *TrackList::Add<TimeTrack>(std::unique_ptr<TimeTrack> &&);
#if defined(USE_MIDI)
template Track *TrackList::Add<NoteTrack>(std::unique_ptr<NoteTrack> &&);
#endif
template Track *TrackList::Add<WaveTrack>(std::unique_ptr<WaveTrack> &&);
template Track *TrackList::Add<LabelTrack>(std::unique_ptr<LabelTrack> &&);
template Track *TrackList::Add<Track>(std::unique_ptr<Track> &&);

template<typename TrackKind>
Track *TrackList::AddToHead(std::unique_ptr<TrackKind> &&t)
{
   Track *pTrack;
   push_front(ListOfTracks::value_type(pTrack = t.release()));
   auto n = getBegin();
   pTrack->SetOwner(mSelf, n);
   pTrack->SetId( TrackId{ ++sCounter } );
   RecalcPositions(n);
   ResizingEvent(n);
   return front().get();
}

// Make instantiations for the linker to find
template Track *TrackList::AddToHead<TimeTrack>(std::unique_ptr<TimeTrack> &&);

template<typename TrackKind>
Track *TrackList::Add(std::shared_ptr<TrackKind> &&t)
{
   push_back(t);

   auto n = getPrev( getEnd() );

   t->SetOwner(mSelf, n);
   t->SetId( TrackId{ ++sCounter } );
   RecalcPositions(n);
   ResizingEvent(n);
   return back().get();
}

// Make instantiations for the linker to find
template Track *TrackList::Add<Track>(std::shared_ptr<Track> &&);
template Track *TrackList::Add<WaveTrack>(std::shared_ptr<WaveTrack> &&);

auto TrackList::Replace(Track * t, ListOfTracks::value_type &&with) ->
   ListOfTracks::value_type
{
   ListOfTracks::value_type holder;
   if (t && with) {
      auto node = t->GetNode();
      t->SetOwner({}, {});

      holder = std::move(*node.first);

      Track *pTrack = with.get();
      *node.first = std::move(with);
      pTrack->SetOwner(mSelf, node);
      pTrack->SetId( t->GetId() );
      RecalcPositions(node);

      DeletionEvent();
      ResizingEvent(node);
   }
   return holder;
}

TrackNodePointer TrackList::Remove(Track *t)
{
   auto result = getEnd();
   if (t) {
      auto node = t->GetNode();
      t->SetOwner({}, {});

      if ( !isNull( node ) ) {
         ListOfTracks::value_type holder = std::move( *node.first );

         result = getNext( node );
         erase(node.first);
         if ( !isNull( result ) )
            RecalcPositions(result);

         DeletionEvent();
      }
   }
   return result;
}

void TrackList::Clear(bool sendEvent)
{
   // Null out the back-pointers in tracks, in case there are outstanding
   // shared_ptrs to those tracks.
   for ( auto pTrack: *this )
      pTrack->SetOwner( {}, {} );
   for ( auto pTrack: mPendingUpdates )
      pTrack->SetOwner( {}, {} );

   ListOfTracks tempList;
   tempList.swap( *this );

   ListOfTracks updating;
   updating.swap( mPendingUpdates );

   mUpdaters.clear();

   if (sendEvent)
      DeletionEvent();
}

/// Return a track in the list that comes after Track t
Track *TrackList::GetNext(Track * t, bool linked) const
{
   if (t) {
      auto node = t->GetNode();
      if ( !isNull( node ) ) {
         if ( linked && t->GetLinked() )
            node = getNext( node );

         if ( !isNull( node ) )
            node = getNext( node );

         if ( !isNull( node ) )
            return node.first->get();
      }
   }

   return nullptr;
}

Track *TrackList::GetPrev(Track * t, bool linked) const
{
   if (t) {
      TrackNodePointer prev;
      auto node = t->GetNode();
      if ( !isNull( node ) ) {
         // linked is true and input track second in team?
         if (linked) {
            prev = getPrev( node );
            if( !isNull( prev ) &&
                !t->GetLinked() && t->GetLink() )
               // Make it the first
               node = prev;
         }

         prev = getPrev( node );
         if ( !isNull( prev ) ) {
            // Back up once
            node = prev;

            // Back up twice sometimes when linked is true
            if (linked) {
               prev = getPrev( node );
               if( !isNull( prev ) &&
                   !(*node.first)->GetLinked() && (*node.first)->GetLink() )
                  node = prev;
            }

            return node.first->get();
         }
      }
   }

   return nullptr;
}

/// For mono track height of track
/// For stereo track combined height of both channels.
int TrackList::GetGroupHeight(const Track * t) const
{
   return Channels(t).sum( &Track::GetHeight );
}

bool TrackList::CanMoveUp(Track * t) const
{
   return GetPrev(t, true) != NULL;
}

bool TrackList::CanMoveDown(Track * t) const
{
   return GetNext(t, true) != NULL;
}

// This is used when you want to swap the channel group starting
// at s1 with that starting at s2.
// The complication is that the tracks are stored in a single
// linked list.
void TrackList::SwapNodes(TrackNodePointer s1, TrackNodePointer s2)
{
   // if a null pointer is passed in, we want to know about it
   wxASSERT(!isNull(s1));
   wxASSERT(!isNull(s2));

   // Deal with first track in each team
   s1 = ( * FindLeader( s1.first->get() ) )->GetNode();
   s2 = ( * FindLeader( s2.first->get() ) )->GetNode();

   // Safety check...
   if (s1 == s2)
      return;

   // Be sure s1 is the earlier iterator
   if ((*s1.first)->GetIndex() >= (*s2.first)->GetIndex())
      std::swap(s1, s2);

   // For saving the removed tracks
   using Saved = std::vector< ListOfTracks::value_type >;
   Saved saved1, saved2;

   auto doSave = [&] ( Saved &saved, TrackNodePointer &s ) {
      size_t nn = Channels( s.first->get() ).size();
      saved.resize( nn );
      // Save them in backwards order
      while( nn-- )
         saved[nn] = std::move( *s.first ), s.first = erase(s.first);
   };

   doSave( saved1, s1 );
   // The two ranges are assumed to be disjoint but might abut
   const bool same = (s1 == s2);
   doSave( saved2, s2 );
   if (same)
      // Careful, we invalidated s1 in the second doSave!
      s1 = s2;

   // Reinsert them
   auto doInsert = [&] ( Saved &saved, TrackNodePointer &s ) {
      Track *pTrack;
      for (auto & pointer : saved)
         pTrack = pointer.get(),
         // Insert before s, and reassign s to point at the new node before
         // old s; which is why we saved pointers in backwards order
         pTrack->SetOwner(mSelf,
                          s = { insert(s.first, std::move(pointer)), this } );
   };
   // This does not invalidate s2 even when it equals s1:
   doInsert( saved2, s1 );
   // Even if s2 was same as s1, this correctly inserts the saved1 range
   // after the saved2 range, when done after:
   doInsert( saved1, s2 );

   // Now correct the Index in the tracks, and other things
   RecalcPositions(s1);
   PermutationEvent();
}

bool TrackList::MoveUp(Track * t)
{
   if (t) {
      Track *p = GetPrev(t, true);
      if (p) {
         SwapNodes(p->GetNode(), t->GetNode());
         return true;
      }
   }

   return false;
}

bool TrackList::MoveDown(Track * t)
{
   if (t) {
      Track *n = GetNext(t, true);
      if (n) {
         SwapNodes(t->GetNode(), n->GetNode());
         return true;
      }
   }

   return false;
}

bool TrackList::Contains(const Track * t) const
{
   return make_iterator_range( *this ).contains( t );
}

bool TrackList::empty() const
{
   return begin() == end();
}

size_t TrackList::size() const
{
   int cnt = 0;

   if (!empty())
      cnt = getPrev( getEnd() ).first->get()->GetIndex() + 1;

   return cnt;
}

TimeTrack *TrackList::GetTimeTrack()
{
   return *Any<TimeTrack>().begin();
}

const TimeTrack *TrackList::GetTimeTrack() const
{
   return const_cast<TrackList*>(this)->GetTimeTrack();
}

unsigned TrackList::GetNumExportChannels(bool selectionOnly) const
{
   /* counters for tracks panned different places */
   int numLeft = 0;
   int numRight = 0;
   //int numMono = 0;
   /* track iteration kit */

   // Want only unmuted wave tracks.
   for (auto tr :
         Any< const WaveTrack >()
            + (selectionOnly ? &Track::IsSelected : &Track::Any)
            - &WaveTrack::GetMute
   ) {
      // Found a left channel
      if (tr->GetChannel() == Track::LeftChannel) {
         numLeft++;
      }

      // Found a right channel
      else if (tr->GetChannel() == Track::RightChannel) {
         numRight++;
      }

      // Found a mono channel, but it may be panned
      else if (tr->GetChannel() == Track::MonoChannel) {
         float pan = tr->GetPan();

         // Figure out what kind of channel it should be
         if (pan == -1.0) {   // panned hard left
            numLeft++;
         }
         else if (pan == 1.0) {  // panned hard right
            numRight++;
         }
         else if (pan == 0) { // panned dead center
            // numMono++;
         }
         else {   // panned somewhere else
            numLeft++;
            numRight++;
         }
      }
   }

   // if there is stereo content, report 2, else report 1
   if (numRight > 0 || numLeft > 0) {
      return 2;
   }

   return 1;
}

namespace {
   template<typename Array, typename TrackRange>
   Array GetTypedTracks(const TrackRange &trackRange,
                       bool selectionOnly, bool includeMuted)
   {
      Array array;

      using Type = typename
         std::remove_reference< decltype( *array[0] ) >::type;
      auto subRange =
         trackRange.template Filter<Type>();
      if ( selectionOnly )
         subRange = subRange + &Track::IsSelected;
      if ( ! includeMuted )
         subRange = subRange - &Type::GetMute;
      std::transform(
         subRange.begin(), subRange.end(), std::back_inserter( array ),
         []( Type *t ){ return Track::Pointer<Type>( t ); }
      );

      return array;
   }
}

WaveTrackArray TrackList::GetWaveTrackArray(bool selectionOnly, bool includeMuted)
{
   return GetTypedTracks<WaveTrackArray>(Any(), selectionOnly, includeMuted);
}

WaveTrackConstArray TrackList::GetWaveTrackConstArray(bool selectionOnly, bool includeMuted) const
{
   return GetTypedTracks<WaveTrackConstArray>(Any(), selectionOnly, includeMuted);
}

#if defined(USE_MIDI)
NoteTrackConstArray TrackList::GetNoteTrackConstArray(bool selectionOnly) const
{
   return GetTypedTracks<NoteTrackConstArray>(Any(), selectionOnly, true);
}
#endif

int TrackList::GetHeight() const
{
   int height = 0;

   if (!empty()) {
      auto track = getPrev( getEnd() ).first->get();
      height = track->GetY() + track->GetHeight();
   }

   return height;
}

namespace {
   // Abstract the common pattern of the following three member functions
   inline double Accumulate
      (const TrackList &list,
       double (Track::*memfn)() const,
       double ident,
       const double &(*combine)(const double&, const double&))
   {
      // Default the answer to zero for empty list
      if (list.empty()) {
         return 0.0;
      }

      // Otherwise accumulate minimum or maximum of track values
      return list.Any().accumulate(ident, combine, memfn);
   }
}

double TrackList::GetMinOffset() const
{
   return Accumulate(*this, &Track::GetOffset, DBL_MAX, std::min);
}

double TrackList::GetStartTime() const
{
   return Accumulate(*this, &Track::GetStartTime, DBL_MAX, std::min);
}

double TrackList::GetEndTime() const
{
   return Accumulate(*this, &Track::GetEndTime, -DBL_MAX, std::max);
}

std::shared_ptr<Track>
TrackList::RegisterPendingChangedTrack( Updater updater, Track *src )
{
   std::shared_ptr<Track> pTrack;
   if (src)
      // convert from unique_ptr to shared_ptr
      pTrack.reset( src->Duplicate().release() );

   if (pTrack) {
      mUpdaters.push_back( updater );
      mPendingUpdates.push_back( pTrack );
      auto n = mPendingUpdates.end();
      --n;
      pTrack->SetOwner(mSelf, {n, &mPendingUpdates});
   }

   return pTrack;
}

void TrackList::RegisterPendingNewTrack( const std::shared_ptr<Track> &pTrack )
{
   auto copy = pTrack;
   Add<Track>( std::move( copy ) );
   pTrack->SetId( TrackId{} );
}

void TrackList::UpdatePendingTracks()
{
   auto pUpdater = mUpdaters.begin();
   for (const auto &pendingTrack : mPendingUpdates) {
      // Copy just a part of the track state, according to the update
      // function
      const auto &updater = *pUpdater;
      auto src = FindById( pendingTrack->GetId() );
      if (pendingTrack && src) {
         if (updater)
            updater( *pendingTrack, *src );
         pendingTrack->DoSetY(src->GetY());
         pendingTrack->DoSetHeight(src->GetHeight());
         pendingTrack->DoSetMinimized(src->GetMinimized());
         pendingTrack->DoSetLinked(src->GetLinked());
      }
      ++pUpdater;
   }
}

void TrackList::ClearPendingTracks( ListOfTracks *pAdded )
// NOFAIL-GUARANTEE
{
   for (const auto &pTrack: mPendingUpdates)
      pTrack->SetOwner( {}, {} );
   mPendingUpdates.clear();
   mUpdaters.clear();

   if (pAdded)
      pAdded->clear();

   for (auto it = ListOfTracks::begin(), stop = ListOfTracks::end();
        it != stop;) {
      if (it->get()->GetId() == TrackId{}) {
         if (pAdded)
            pAdded->push_back( *it );
         (*it)->SetOwner( {}, {} );
         it = erase( it );
      }
      else
         ++it;
   }

   if (!empty())
      RecalcPositions(getBegin());
}

bool TrackList::ApplyPendingTracks()
{
   bool result = false;

   ListOfTracks additions;
   ListOfTracks updates;
   {
      // Always clear, even if one of the update functions throws
      auto cleanup = finally( [&] { ClearPendingTracks( &additions ); } );
      UpdatePendingTracks();
      updates.swap( mPendingUpdates );
   }

   // Remaining steps must be NOFAIL-GUARANTEE so that this function
   // gives STRONG-GUARANTEE

   std::vector< std::shared_ptr<Track> > reinstated;

   for (auto &pendingTrack : updates) {
      if (pendingTrack) {
         auto src = FindById( pendingTrack->GetId() );
         if (src)
            this->Replace(src, std::move(pendingTrack)), result = true;
         else
            // Perhaps a track marked for pending changes got deleted by
            // some other action.  Recreate it so we don't lose the
            // accumulated changes.
            reinstated.push_back(pendingTrack);
      }
   }

   // If there are tracks to reinstate, append them to the list.
   for (auto &pendingTrack : reinstated)
      if (pendingTrack)
         this->Add(std::move(pendingTrack)), result = true;

   // Put the pending added tracks back into the list, preserving their
   // positions.
   bool inserted = false;
   ListOfTracks::iterator first;
   for (auto &pendingTrack : additions) {
      if (pendingTrack) {
         auto iter = ListOfTracks::begin();
         std::advance( iter, pendingTrack->GetIndex() );
         iter = ListOfTracks::insert( iter, pendingTrack );
         pendingTrack->SetOwner( mSelf, {iter, this} );
         pendingTrack->SetId( TrackId{ ++sCounter } );
         if (!inserted) {
            first = iter;
            inserted = true;
         }
      }
   }
   if (inserted) {
      RecalcPositions({first, this});
      result = true;
   }

   return result;
}

std::shared_ptr<const Track> Track::SubstitutePendingChangedTrack() const
{
   // Linear search.  Tracks in a project are usually very few.
   auto pList = mList.lock();
   if (pList) {
      const auto id = GetId();
      const auto end = pList->mPendingUpdates.end();
      auto it = std::find_if(
         pList->mPendingUpdates.begin(), end,
         [=](const ListOfTracks::value_type &ptr){ return ptr->GetId() == id; } );
      if (it != end)
         return *it;
   }
   return Pointer( this );
}

bool TrackList::HasPendingTracks() const
{
   if ( !mPendingUpdates.empty() )
      return true;
   if (end() != std::find_if(begin(), end(), [](const Track *t){
      return t->GetId() == TrackId{};
   }))
      return true;
   return false;
}

#include "AudioIO.h"
TransportTracks GetAllPlaybackTracks(TrackList &trackList, bool selectedOnly, bool useMidi)
{
   TransportTracks result;
   result.playbackTracks = trackList.GetWaveTrackArray(selectedOnly);
#ifdef EXPERIMENTAL_MIDI_OUT
   if (useMidi)
      result.midiTracks = trackList.GetNoteTrackConstArray(selectedOnly);
#else
   WXUNUSED(useMidi);
#endif
   return result;
}

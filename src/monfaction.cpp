#include "debug.h"
#include "monfaction.h"
#include "json.h"
#include <vector>
#include <queue>

std::map<std::string, mf_attitude> mf_attitude_map;

std::unordered_map< mfaction_str_id, mfaction_id > faction_map;
std::vector< monfaction > faction_list;

void add_to_attitude_map( const std::set< std::string > &keys, mfaction_att_map &map,
                          mf_attitude value );

void apply_base_faction( const monfaction &base, monfaction &faction );

template<>
const monfaction &int_id<monfaction>::obj() const
{
    if( static_cast<size_t>( _id ) >= faction_list.size() ) {
        debugmsg( "invalid monfaction id %d", _id );
        return mfaction_id( 0 ).obj();
    }

    return faction_list[_id];
}

template<>
string_id<monfaction> int_id<monfaction>::id() const
{
    return obj().name;
}

template<>
int_id<monfaction> string_id<monfaction>::id() const
{
    const auto &iter = faction_map.find( *this );
    if( iter == faction_map.end() ) {
        debugmsg( "invalid monfaction id %s", c_str() );
        return mfaction_id( 0 );
    }
    return iter->second;
}

template<>
const monfaction &string_id<monfaction>::obj() const
{
    return id().obj();
}

template<>
bool string_id<monfaction>::is_valid() const
{
    return faction_map.count( *this ) > 0;
}

template<>
int_id<monfaction>::int_id( const string_id<monfaction> &id )
: _id( id.id() )
{
}

mfaction_id monfactions::get_or_add_faction( const std::string &name_arg )
{
    const mfaction_str_id name( name_arg );
    auto found = faction_map.find( name );
    if( found == faction_map.end() ) {
        monfaction mfact;
        mfact.name = name;
        mfact.id = mfaction_id( faction_map.size() );
        // -1 base faction marks this faction as not initialized.
        // If it is not changed before validation, it will become a child of
        // the root of the faction tree.
        mfact.base_faction = mfaction_id( -1 );
        faction_map[mfact.name] = mfact.id;
        faction_list.push_back( mfact );
        found = faction_map.find( mfact.name );
    }

    return found->second;
}

void apply_base_faction( mfaction_id base, mfaction_id faction_id )
{
    for( const auto &pair : base.obj().attitude_map ) {
        // Fill in values set in base faction, but not in derived one
        auto &faction = faction_list[faction_id];
        if( faction.attitude_map.count( pair.first ) == 0 ) {
            faction.attitude_map.insert( pair );
        }
    }
}

mf_attitude monfaction::attitude( const mfaction_id &other ) const
{
    const auto &found = attitude_map.find( other );
    if( found != attitude_map.end() ) {
        return found->second;
    }

    const auto base = other.obj().base_faction;
    if( other != base ) {
        return attitude( base );
    }

    // Shouldn't happen
    debugmsg( "Invalid faction relations (no relation found): %s -> %s",
              name.c_str(), other.obj().name.c_str() );
    return MFA_FRIENDLY;
}

void monfactions::finalize_monfactions()
{
    if( faction_list.empty() ) {
        debugmsg( "No monster factions found." );
        return;
    }

    // Create a tree of faction dependence
    std::multimap< mfaction_id, mfaction_id > child_map;
    std::set< mfaction_id > unloaded; // To check if cycles exist
    std::queue< mfaction_id > queue;
    for( auto &faction : faction_list ) {
        unloaded.insert( faction.id );
        if( faction.id == faction.base_faction ) {
            // No parent = root of the (a?) tree
            queue.push( faction.id );
            continue;
        }

        // Point parent to children
        if( faction.base_faction >= 0 ) {
            child_map.insert( std::make_pair( faction.base_faction, faction.id ) );
        }

        // Set faction as friendly to itself if not explicitly set to anything
        if( faction.attitude_map.count( faction.id ) == 0 ) {
            faction.attitude_map[faction.id] = MFA_FRIENDLY;
        }
    }

    if( queue.empty() && !faction_list.empty() ) {
        debugmsg( "No valid root monster faction!" );
        return;
    }

    // Set uninitialized factions to be children of the root.
    // If more than one root exists, use the first one.
    const auto root = queue.front();
    for( auto &faction : faction_list ) {
        if( faction.base_faction < 0 ) {
            faction.base_faction = root;
            // If it is the (new) root, connecting it to own parent (self) would create a cycle.
            // So only try to connect it to the parent if it isn't own parent.
            if( faction.base_faction != faction.id ) {
                child_map.insert( std::make_pair( faction.base_faction, faction.id ) );
            }
        }
    }

    // Traverse the tree (breadth-first), starting from root
    while( !queue.empty() ) {
        mfaction_id cur = queue.front();
        queue.pop();
        if( unloaded.count( cur ) != 0 ) {
            unloaded.erase( cur );
        } else {
            debugmsg( "Tried to load monster faction %s more than once", cur.obj().name.c_str() );
            continue;
        }
        auto children = child_map.equal_range( cur );
        for( auto &it = children.first; it != children.second; ++it ) {
            // Copy attributes to child
            apply_base_faction( cur, it->second );
            queue.push( it->second );
        }
    }

    // Bad json
    if( !unloaded.empty() ) {
        std::string names;
        for( auto &fac : unloaded ) {
            names.append( fac.id().str() );
            names.append( " " );
            auto &the_faction = faction_list[fac];
            the_faction.base_faction = root;
        }

        debugmsg( "Cycle encountered when processing monster factions. Bad factions:\n %s", names.c_str() );
    }
}

// Get pointers to factions from 'keys' and add them to 'map' with value == 'value'
void add_to_attitude_map( const std::set< std::string > &keys, mfaction_att_map &map,
                                            mf_attitude value )
{
    for( const auto &k : keys ) {
        const auto &faction = monfactions::get_or_add_faction( k );
        map[faction] = value;
    }
}

void monfactions::load_monster_faction(JsonObject &jo)
{
    // Factions inherit values from their parent factions - this is set during finalization
    std::string name = jo.get_string( "name" );
    monfaction &faction = faction_list[get_or_add_faction( name )];
    std::string base_faction = jo.get_string( "base_faction", "" );
    faction.base_faction = get_or_add_faction( base_faction );
    std::set< std::string > by_mood, neutral, friendly;
    by_mood = jo.get_tags( "by_mood" );
    neutral = jo.get_tags( "neutral" );
    friendly = jo.get_tags( "friendly" );
    add_to_attitude_map( by_mood, faction.attitude_map, MFA_BY_MOOD );
    add_to_attitude_map( neutral, faction.attitude_map, MFA_NEUTRAL );
    add_to_attitude_map( friendly, faction.attitude_map, MFA_FRIENDLY );
}

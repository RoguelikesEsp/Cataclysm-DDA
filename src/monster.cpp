#include "monster.h"
#include "map.h"
#include "map_iterator.h"
#include "mondeath.h"
#include "output.h"
#include "game.h"
#include "debug.h"
#include "rng.h"
#include "item.h"
#include "translations.h"
#include "overmapbuffer.h"
#include <sstream>
#include <fstream>
#include <stdlib.h>
#include <algorithm>
#include "cursesdef.h"
#include "monstergenerator.h"
#include "json.h"
#include "messages.h"
#include "mondefense.h"
#include "mission.h"
#include "mongroup.h"
#include "monfaction.h"
#include "options.h"
#include "trap.h"
#include "line.h"

#define SGN(a) (((a)<0) ? -1 : 1)
#define SQR(a) ((a)*(a))

monster::monster()
{
 position.x = 20;
 position.y = 10;
 position.z = -500; // Some arbitrary number that will cause debugmsgs
 wandf = 0;
 hp = 60;
 moves = 0;
 def_chance = 0;
 friendly = 0;
 anger = 0;
 morale = 2;
 last_loaded = 0;
 faction = mfaction_id( 0 );
 mission_id = -1;
 no_extra_death_drops = false;
 dead = false;
 made_footstep = false;
 unique_name = "";
 hallucination = false;
 ignoring = 0;
}

monster::monster(mtype *t)
{
 position.x = 20;
 position.y = 10;
 position.z = -500; // Some arbitrary number that will cause debugmsgs
 wandf = 0;
 type = t;
 moves = type->speed;
 Creature::set_speed_base(type->speed);
 hp = type->hp;
 for( auto &elem : type->sp_freq ) {
     sp_timeout.push_back( rng( 0, elem ) );
 }
 def_chance = type->def_chance;
 friendly = 0;
 anger = t->agro;
 morale = t->morale;
 last_loaded = 0;
 faction = t->default_faction;
 mission_id = -1;
 no_extra_death_drops = false;
 dead = false;
 made_footstep = false;
 unique_name = "";
 hallucination = false;
 ignoring = 0;
 ammo = t->starting_ammo;
}

monster::monster(mtype *t, const tripoint &p )
{
 position = p;
 wandf = 0;
 type = t;
 moves = type->speed;
 Creature::set_speed_base(type->speed);
 hp = type->hp;
 for( auto &elem : type->sp_freq ) {
     sp_timeout.push_back( elem );
 }
 def_chance = type->def_chance;
 friendly = 0;
 anger = type->agro;
 morale = type->morale;
 faction = t->default_faction;
 last_loaded = 0;
 mission_id = -1;
 no_extra_death_drops = false;
 dead = false;
 made_footstep = false;
 unique_name = "";
 hallucination = false;
 ignoring = 0;
 ammo = t->starting_ammo;
}

monster::~monster()
{
}

void monster::setpos( const tripoint &p )
{
    if( p == pos3() ) {
        return;
    }

    g->update_zombie_pos( *this, p );
    position = p;
}

const tripoint &monster::pos() const
{
    return position;
}

void monster::poly(mtype *t)
{
    double hp_percentage = double(hp) / double(type->hp);
    type = t;
    moves = 0;
    Creature::set_speed_base(type->speed);
    anger = type->agro;
    morale = type->morale;
    hp = int(hp_percentage * type->hp);
    sp_timeout.clear();
    for( auto &elem : type->sp_freq ) {
        sp_timeout.push_back( elem );
    }
    def_chance = type->def_chance;
    faction = t->default_faction;
}

bool monster::can_upgrade() const
{
    // If we don't upgrade
    if ((type->half_life <= 0 && type->base_upgrade_chance <= 0) ||
        (type->upgrade_group == mongroup_id( "GROUP_NULL" ) && type->upgrades_into == "NULL")) {
        return false;
    }
    // Or we aren't allowed to yet
    if (ACTIVE_WORLD_OPTIONS["MONSTER_UPGRADE_FACTOR"] <= 0) {
        return false;
    } else {
        if ((calendar::turn.get_turn() / DAYS(1)) <
             (type->upgrade_min / ACTIVE_WORLD_OPTIONS["MONSTER_UPGRADE_FACTOR"])) {
            return false;
        }
    }
    return true;
}

void monster::update_check() {
    // Hallucinations don't upgrade!
    if (is_hallucination()) {
        return;
    }

    // No chance of upgrading, abort
    if ((type->half_life <= 0 && type->base_upgrade_chance <= 0) ||
        (type->upgrade_group == mongroup_id( "GROUP_NULL" ) && type->upgrades_into == "NULL")) {
        return;
    }
    int current_day = calendar::turn.get_turn()/ DAYS(1);
    int upgrade_time = 0;
    if (ACTIVE_WORLD_OPTIONS["MONSTER_UPGRADE_FACTOR"] > 0) {
        upgrade_time = type->upgrade_min / ACTIVE_WORLD_OPTIONS["MONSTER_UPGRADE_FACTOR"];
    } else {
        // Should ensure that the monsters never upgrade
        upgrade_time = current_day + 1;
    }
    add_msg(m_debug, "Current:day: %d", current_day);
    add_msg(m_debug, "Upgrade time : %d", upgrade_time);
    add_msg(m_debug, "Last loaded: %d", last_loaded);

    if (current_day == last_loaded || current_day < upgrade_time) {
        add_msg(m_debug, "Upgrade time less");
        last_loaded = current_day;
        return;
    }

    // We don't start counting until the minimum upgrade time
    int time_passed = current_day - std::max(last_loaded, upgrade_time);
    add_msg(m_debug, "Time passed: %d", time_passed);

    float upgrade_chance = 0;
    // If we have a valid half life use a radioactive decay function. This will become
    // rapidly inaccurate when half_life > 700, if longer half lives are needed increase
    // the 1000 here, the 10 in the else, and the rng() range by factors of 10 as necessary.
    if (type->half_life > 0) {
        float elapsed_lives = float(time_passed) / float(type->half_life);
        // (1- (.5 - base%)^lives) = percentage that have upgraded
        upgrade_chance = 1000 * (1 - pow(std::max(0.0, 0.5 - type->base_upgrade_chance * .01 ),
                                               elapsed_lives));
    } else {
        // Not a valid half life, so just do base_upgrade_chance percent per day
        // (1 - (1 - base%)^days) = percentage that has upgraded
        upgrade_chance = 1000 * (1 - pow(1 - type->base_upgrade_chance * .01, time_passed));
    }
    add_msg(m_debug, "Upgrade chance: %f", upgrade_chance);
    if (upgrade_chance > rng(0, 999)){
        // Try to upgrade to a single monster first
        if (type->upgrades_into != "NULL"){
            poly(GetMType(type->upgrades_into));
        // Else upgrade to the desired group
        } else {
            const auto monsters = MonsterGroupManager::GetMonstersFromGroup(type->upgrade_group);
            const std::string newtype = monsters[rng(0, monsters.size() - 1)];
            poly(GetMType(newtype));
        }
    }

    last_loaded = current_day;
}

void monster::spawn(const tripoint &p)
{
    position = p;
}

std::string monster::name(unsigned int quantity) const
{
 if (!type) {
  debugmsg ("monster::name empty type!");
  return std::string();
 }
 if (unique_name != "")
  return string_format("%s: %s",
                       (type->nname(quantity).c_str()), unique_name.c_str());
 return type->nname(quantity);
}

// MATERIALS-TODO: put description in materials.json?
std::string monster::name_with_armor() const
{
    std::string ret;
    if( type->in_species("INSECT") ) {
        ret = string_format(_("carapace"));
    } else if( type->has_material("veggy") ) {
        ret = string_format(_("thick bark"));
    } else if( type->has_material("flesh") || type->has_material("hflesh") ||
               type->has_material("iflesh") ) {
        ret = string_format(_("thick hide"));
    } else if( type->has_material("iron") || type->has_material("steel")) {
        ret = string_format(_("armor plating"));
    } else if( type->has_material("protoplasmic") ) {
        ret = string_format(_("hard protoplasmic hide"));
    }
    return ret;
}

std::string monster::disp_name(bool possessive) const {
    if (!possessive) {
        return string_format(_("the %s"), name().c_str());
    } else {
        return string_format(_("the %s's"), name().c_str());
    }
}

std::string monster::skin_name() const {
    return name_with_armor();
}

void monster::get_HP_Bar(nc_color &color, std::string &text) const
{
    std::tie(text, color) = ::get_hp_bar(hp, type->hp, true);
}

void monster::get_Attitude(nc_color &color, std::string &text) const
{
    switch (attitude(&(g->u))) {
        case MATT_FRIEND:
            color = h_white;
            text = _("Friendly ");
            break;
        case MATT_FPASSIVE:
            color = h_white;
            text = _("Passive ");
            break;
        case MATT_FLEE:
            color = c_green;
            text = _("Fleeing! ");
            break;
        case MATT_IGNORE:
            color = c_ltgray;
            text = _("Ignoring ");
            break;
        case MATT_FOLLOW:
            color = c_yellow;
            text = _("Tracking ");
            break;
        case MATT_ATTACK:
            color = c_red;
            text = _("Hostile! ");
            break;
        case MATT_ZLAVE:
            color = c_green;
            text = _("Zombie slave ");
            break;
        default:
            color = h_red;
            text = "BUG: Behavior unnamed. (monster.cpp:get_Attitude)";
            break;
    }
}

int monster::print_info(WINDOW* w, int vStart, int vLines, int column) const
{
    const int vEnd = vStart + vLines;

    mvwprintz(w, vStart++, column, c_white, "%s ", name().c_str());
    nc_color color = c_white;
    std::string attitude = "";

    get_Attitude(color, attitude);
    wprintz(w, color, "%s", attitude.c_str());

    if (has_effect("downed")) {
        wprintz(w, h_white, _("On ground"));
    } else if (has_effect("stunned")) {
        wprintz(w, h_white, _("Stunned"));
    } else if (has_effect("lightsnare") || has_effect("heavysnare") || has_effect("beartrap")) {
        wprintz(w, h_white, _("Trapped"));
    } else if (has_effect("tied")) {
        wprintz(w, h_white, _("Tied"));
    }
    std::string damage_info;
    nc_color col;
    if (hp >= type->hp) {
        damage_info = _("It is uninjured");
        col = c_green;
    } else if (hp >= type->hp * .8) {
        damage_info = _("It is lightly injured");
        col = c_ltgreen;
    } else if (hp >= type->hp * .6) {
        damage_info = _("It is moderately injured");
        col = c_yellow;
    } else if (hp >= type->hp * .3) {
        damage_info = _("It is heavily injured");
        col = c_yellow;
    } else if (hp >= type->hp * .1) {
        damage_info = _("It is severely injured");
        col = c_ltred;
    } else {
        damage_info = _("it is nearly dead");
        col = c_red;
    }
    mvwprintz(w, vStart++, column, col, "%s", damage_info.c_str());

    std::vector<std::string> lines = foldstring(type->description, getmaxx(w) - 1 - column);
    int numlines = lines.size();
    for (int i = 0; i < numlines && vStart <= vEnd; i++) {
        mvwprintz(w, vStart++, column, c_white, "%s", lines[i].c_str());
    }

    return vStart;
}

const std::string &monster::symbol() const
{
    return type->sym;
}

nc_color monster::basic_symbol_color() const
{
    return type->color;
}

nc_color monster::symbol_color() const
{
    return color_with_effects();
}

bool monster::is_symbol_highlighted() const
{
    return (friendly != 0);
}

nc_color monster::color_with_effects() const
{
    nc_color ret = type->color;
    if (has_effect("beartrap") || has_effect("stunned") || has_effect("downed") || has_effect("tied") ||
          has_effect("lightsnare") || has_effect("heavysnare")) {
        ret = hilite(ret);
    }
    if (has_effect("pacified")) {
        ret = invert_color(ret);
    }
    if (has_effect("onfire")) {
        ret = red_background(ret);
    }
    return ret;
}

bool monster::avoid_trap( const tripoint & /* pos */, const trap &tr )
{
    // The trap position is not used, monsters are to stupid to remember traps. Actually, they do
    // not even see them.
    // Traps are on the ground, digging monsters go below, fliers and climbers go above.
    if( digging() || has_flag( MF_FLIES ) ) {
        return true;
    }
    return dice( 3, type->sk_dodge + 1 ) >= dice( 3, tr.get_avoidance() );
}

bool monster::has_flag(const m_flag f) const
{
 return type->has_flag(f);
}

bool monster::can_see() const
{
 return has_flag(MF_SEES) && !has_effect("blind");
}

bool monster::can_hear() const
{
 return has_flag(MF_HEARS) && !has_effect("deaf");
}

bool monster::can_submerge() const
{
  return (has_flag(MF_NO_BREATHE) || has_flag(MF_SWIMS) || has_flag(MF_AQUATIC))
          && !has_flag(MF_ELECTRONIC);
}

bool monster::can_drown() const
{
 return !has_flag(MF_SWIMS) && !has_flag(MF_AQUATIC)
         && !has_flag(MF_NO_BREATHE) && !has_flag(MF_FLIES);
}

bool monster::digging() const
{
    return has_flag(MF_DIGS) || ( has_flag(MF_CAN_DIG) && underwater );
}

bool monster::can_act() const
{
    return moves > 0 && !has_flag(MF_IMMOBILE) &&
        ( effects.empty() ||
          ( !has_effect("stunned") && !has_effect("downed") && !has_effect("webbed") ) );
}


int monster::sight_range( const int light_level ) const
{
    // Non-aquatic monsters can't see much when submerged
    if( !can_see() || ( underwater && !has_flag( MF_SWIMS ) && !has_flag( MF_AQUATIC ) && !digging() ) ) {
        return 1;
    }

    int range = ( light_level * type->vision_day ) +
                ( ( DAYLIGHT_LEVEL - light_level ) * type->vision_night );
    range /= DAYLIGHT_LEVEL;

    return range;
}

bool monster::made_of(std::string m) const
{
    return type->has_material( m );
}

bool monster::made_of(phase_id p) const
{
    return type->phase == p;
}

void monster::load_info(std::string data)
{
    std::stringstream dump;
    dump << data;
    JsonIn jsin(dump);
    try {
        deserialize(jsin);
    } catch (std::string jsonerr) {
        debugmsg("monster:load_info: Bad monster json\n%s", jsonerr.c_str() );
    }
}

void monster::debug(player &u)
{
    debugmsg("monster::debug %s has %d steps planned.", name().c_str(), plans.size());
    debugmsg("monster::debug %s Moves %d Speed %d HP %d",name().c_str(), moves, get_speed(), hp);
    for (size_t i = 0; i < plans.size(); i++) {
        const int digit = '0' + (i % 10);
        mvaddch(plans[i].y - SEEY + u.posy(), plans[i].x - SEEX + u.posx(), digit);
    }
    getch();
}

void monster::shift(int sx, int sy)
{
    const int xshift = sx * SEEX;
    const int yshift = sy * SEEY;
    position.x -= xshift;
    position.y -= yshift;
    for (auto &i : plans) {
        i.x -= xshift;
        i.y -= yshift;
    }

    if( wandf > 0 ) {
        wander_pos.x -= xshift;
        wander_pos.y -= yshift;
    }
}

tripoint monster::move_target()
{
    if( plans.empty() ) {
        // if we have no plans, pretend it's intentional
        return pos3();
    }
    return plans.back();
}

Creature *monster::attack_target()
{
    if( plans.empty() ) {
        return nullptr;
    }

    Creature *target = g->critter_at( move_target() );
    if( target == nullptr || target == this ||
        attitude_to( *target ) == Creature::A_FRIENDLY || !sees(*target) ) {
        return nullptr;
    }

    return target;
}

bool monster::is_fleeing(player &u) const
{
 if (has_effect("run"))
  return true;
 monster_attitude att = attitude(&u);
 return (att == MATT_FLEE ||
         (att == MATT_FOLLOW && rl_dist( pos3(), u.pos3() ) <= 4));
}

Creature::Attitude monster::attitude_to( const Creature &other ) const
{
    const auto m = dynamic_cast<const monster *>( &other );
    const auto p = dynamic_cast<const player *>( &other );
    if( m != nullptr ) {
        if( m == this ) {
            return A_FRIENDLY;
        }
        auto faction_att = faction.obj().attitude( m->faction );
        if( ( friendly != 0 && m->friendly != 0 ) ||
            ( friendly == 0 && m->friendly == 0 && faction_att == MFA_FRIENDLY ) ) {
            // Friendly (to player) monsters are friendly to each other
            // Unfriendly monsters go by faction attitude
            return A_FRIENDLY;
        } else if( ( friendly == 0 && m->friendly == 0 && faction_att == MFA_NEUTRAL ) ||
                     morale < 0 || anger < 10 ) {
            // Stuff that won't attack is neutral to everything
            return A_NEUTRAL;
        } else {
            return A_HOSTILE;
        }
    } else if( p != nullptr ) {
        switch( attitude( const_cast<player *>( p ) ) ) {
            case MATT_FRIEND:
                return A_FRIENDLY;
            case MATT_FPASSIVE:
            case MATT_FLEE:
            case MATT_IGNORE:
            case MATT_FOLLOW:
            case MATT_ZLAVE:
                return A_NEUTRAL;
            case MATT_ATTACK:
                return A_HOSTILE;
            case MATT_NULL:
            case NUM_MONSTER_ATTITUDES:
                break;
        }
    }
    // Should not happen!, creature should be either player or monster
    return A_NEUTRAL;
}

monster_attitude monster::attitude(player *u) const
{
    if( friendly != 0 ) {
        if( has_effect( "docile" ) ) {
            return MATT_FPASSIVE;
        }
        if( u == &g->u ) {
            return MATT_FRIEND;
        }
        // Zombies don't understand not attacking NPCs, but dogs and bots should.
        npc *np = dynamic_cast< npc* >( u );
        if( np != nullptr && np->attitude != NPCATT_KILL && !type->in_species( "ZOMBIE" ) ) {
            return MATT_FRIEND;
        }
    }
    if (has_effect("run")) {
        return MATT_FLEE;
    }
    if (has_effect("pacified")) {
        return MATT_ZLAVE;
    }

    int effective_anger  = anger;
    int effective_morale = morale;

    if (u != NULL) {
        if (((type->in_species("MAMMAL") && u->has_trait("PHEROMONE_MAMMAL")) ||
             (type->in_species("INSECT") && u->has_trait("PHEROMONE_INSECT"))) &&
            effective_anger >= 10) {
            effective_anger -= 20;
        }

        if ( (type->id == "mon_bee") && (u->has_trait("FLOWERS"))) {
            effective_anger -= 10;
        }

        if (u->has_trait("TERRIFYING")) {
            effective_morale -= 10;
        }

        if (u->has_trait("ANIMALEMPATH") && has_flag(MF_ANIMAL)) {
            if (effective_anger >= 10) {
                effective_anger -= 10;
            }
            if (effective_anger < 10) {
                effective_morale += 5;
            }
        }
        if (u->has_trait("ANIMALDISCORD") && has_flag(MF_ANIMAL)) {
            if (effective_anger >= 10) {
                effective_anger += 10;
            }
            if (effective_anger < 10) {
                effective_morale -= 5;
            }
        }
    }

    if (effective_morale < 0) {
        if (effective_morale + effective_anger > 0) {
            return MATT_FOLLOW;
        }
        return MATT_FLEE;
    }

    if (effective_anger <= 0) {
        return MATT_IGNORE;
    }

    if (effective_anger < 10) {
        return MATT_FOLLOW;
    }

    return MATT_ATTACK;
}

int monster::hp_percentage() const
{
    return ( get_hp( hp_torso ) * 100 ) / get_hp_max();
}

void monster::process_triggers()
{
    anger += trigger_sum( &(type->anger) );
    anger -= trigger_sum( &(type->placate) );
    morale -= trigger_sum( &(type->fear) );
    if( morale != type->morale && one_in( 10 ) ) {
        if( morale < type->morale ) {
            morale++;
        } else {
            morale--;
        }
    }

    if( anger != type->agro && one_in( 10 ) ) {
        if( anger < type->agro ) {
            anger++;
        } else {
            anger--;
        }
    }

    // Cap values at [-100, 100] to prevent perma-angry moose etc.
    morale = std::min( 100, std::max( -100, morale ) );
    anger  = std::min( 100, std::max( -100, anger  ) );
}

// This Adjustes anger/morale levels given a single trigger.
void monster::process_trigger(monster_trigger trig, int amount)
{
    if (type->has_anger_trigger(trig)){
        anger += amount;
    }
    if (type->has_fear_trigger(trig)){
        morale -= amount;
    }
    if (type->has_placate_trigger(trig)){
        anger -= amount;
    }
}


int monster::trigger_sum(std::set<monster_trigger> *triggers) const
{
    int ret = 0;
    bool check_terrain = false, check_meat = false, check_fire = false;
    for( const auto &trigger : *triggers ) {
        switch( trigger ) {
            case MTRIG_STALK:
                if( anger > 0 && one_in( 5 ) ) {
                    ret++;
                }
                break;

            case MTRIG_MEAT:
                // Disable meat checking for now
                // It's hard to ever see it in action
                // and even harder to balance it without making it exploity

                // check_terrain = true;
                // check_meat = true;
                break;

            case MTRIG_FIRE:
                check_terrain = true;
                check_fire = true;
                break;

            default:
                break; // The rest are handled when the impetus occurs
        }
    }

    if( check_terrain ) {
        for( auto &p : g->m.points_in_radius( pos(), 3 ) ) {
            // Note: can_see_items doesn't check actual visibility
            // This will check through walls, but it's too small to matter
            if( check_meat && g->m.sees_some_items( p, *this ) ) {
                auto items = g->m.i_at( p );
                for( auto &item : items ) {
                    if( item.is_corpse() || item.type->id == "meat" ||
                        item.type->id == "meat_cooked" || item.type->id == "human_flesh" ) {
                        ret += 3;
                        check_meat = false;
                    }
                }
            }

            if( check_fire ) {
                ret += 5 * g->m.get_field_strength( p, fd_fire );
            }
        }
    }

    return ret;
}

bool monster::is_underwater() const {
    return underwater && can_submerge();
}

bool monster::is_on_ground() const {
    return false; //TODO: actually make this work
}

bool monster::has_weapon() const {
    return false; // monsters will never have weapons, silly
}

bool monster::is_warm() const {
    return has_flag(MF_WARM);
}

bool monster::is_elec_immune() const
{
    return is_immune_damage( DT_ELECTRIC );
}

bool monster::is_immune_effect( const efftype_id &effect ) const
{
    if( effect == "onfire" ) {
        return is_immune_damage( DT_HEAT );
    }

    return false;
}

bool monster::is_immune_damage( const damage_type dt ) const
{
    switch( dt ) {
    case DT_NULL:
        return true;
    case DT_TRUE:
        return false;
    case DT_BIOLOGICAL: // NOTE: Unused
        return false;
    case DT_BASH:
        return false;
    case DT_CUT:
        return false;
    case DT_ACID:
        return has_flag( MF_ACIDPROOF );
    case DT_STAB:
        return false;
    case DT_HEAT:
        return made_of("steel") || made_of("stone"); // Ugly hardcode - remove later
    case DT_COLD:
        return false;
    case DT_ELECTRIC:
        return type->sp_defense == &mdefense::zapback ||
           has_flag( MF_ELECTRIC );
    default:
        return true;
    }
}

bool monster::is_dead_state() const {
    return hp <= 0;
}

void monster::dodge_hit(Creature *, int) {
}

bool monster::block_hit(Creature *, body_part &, damage_instance &) {
    return false;
}

void monster::absorb_hit(body_part, damage_instance &dam) {
    for( auto &elem : dam.damage_units ) {
        elem.amount -= std::min( resistances( *this ).get_effective_resist( elem ), elem.amount );
    }
}

void monster::melee_attack(Creature &target, bool, const matec_id&) {
    mod_moves(-100);
    if (type->melee_dice == 0) { // We don't attack, so just return
        return;
    }
    add_effect("hit_by_player", 3); // Make us a valid target for a few turns

    if (has_flag(MF_HIT_AND_RUN)) {
        add_effect("run", 4);
    }

    bool u_see_me = g->u.sees(*this);

    body_part bp_hit;
    //int highest_hit = 0;

    damage_instance damage;
    if(!is_hallucination()) {
        if (type->melee_dice > 0) {
            damage.add_damage(DT_BASH,
                    dice(type->melee_dice,type->melee_sides));
        }
        if (type->melee_cut > 0) {
            damage.add_damage(DT_CUT, type->melee_cut);
        }
    }

    dealt_damage_instance dealt_dam;
    int hitspread = target.deal_melee_attack(this, hit_roll());
    if (hitspread >= 0) {
        target.deal_melee_hit(this, hitspread, false, damage, dealt_dam);
    }
    bp_hit = dealt_dam.bp_hit;

    if (hitspread < 0) { // a miss
        // TODO: characters practice dodge when a hit misses 'em
        if (target.is_player()) {
            if (u_see_me) {
                add_msg(_("You dodge %s."), disp_name().c_str());
            } else {
                add_msg(_("You dodge an attack from an unseen source."));
            }
        } else {
            if (u_see_me) {
                add_msg(_("The %1$s dodges %2$s attack."), name().c_str(),
                            target.disp_name(true).c_str());
            }
        }
    //Hallucinations always produce messages but never actually deal damage
    } else if (is_hallucination() || dealt_dam.total_damage() > 0) {
        if (target.is_player()) {
            if (u_see_me) {
                //~ 1$s is attacker name, 2$s is bodypart name in accusative.
                add_msg(m_bad, _("The %1$s hits your %2$s."), name().c_str(),
                        body_part_name_accusative(bp_hit).c_str());
            } else {
                //~ %s is bodypart name in accusative.
                add_msg(m_bad, _("Something hits your %s."),
                        body_part_name_accusative(bp_hit).c_str());
            }
        } else {
            if (u_see_me) {
                //~ 1$s is attacker name, 2$s is target name, 3$s is bodypart name in accusative.
                add_msg(_("The %1$s hits %2$s %3$s."), name().c_str(),
                            target.disp_name(true).c_str(),
                            body_part_name_accusative(bp_hit).c_str());
            }
        }
    } else {
        if (target.is_player()) {
            if (u_see_me) {
                //~ 1$s is attacker name, 2$s is bodypart name in accusative, 3$s is armor name
                add_msg(_("The %1$s hits your %2$s, but your %3$s protects you."), name().c_str(),
                        body_part_name_accusative(bp_hit).c_str(), target.skin_name().c_str());
            } else {
                //~ 1$s is bodypart name in accusative, 2$s is armor name.
                add_msg(_("Something hits your %1$s, but your %2$s protects you."),
                        body_part_name_accusative(bp_hit).c_str(), target.skin_name().c_str());
            }
        } else {
            if (u_see_me) {
                //~ $1s is monster name, %2$s is that monster target name,
                //~ $3s is target bodypart name in accusative, $4s is the monster target name,
                //~ 5$s is target armor name.
                add_msg(_("The %1$s hits %2$s %3$s but is stopped by %4$s %5$s."), name().c_str(),
                            target.disp_name(true).c_str(),
                            body_part_name_accusative(bp_hit).c_str(),
                            target.disp_name(true).c_str(),
                            target.skin_name().c_str());
            }
        }
    }
    target.check_dead_state();

    if (is_hallucination()) {
        if(one_in(7)) {
            die( nullptr );
        }
        return;
    }

    // Adjust anger/morale of same-species monsters, if appropriate
    int anger_adjust = 0, morale_adjust = 0;
    if (type->has_anger_trigger(MTRIG_FRIEND_ATTACKED)){
        anger_adjust += 15;
    }
    if (type->has_fear_trigger(MTRIG_FRIEND_ATTACKED)){
        morale_adjust -= 15;
    }
    if (type->has_placate_trigger(MTRIG_FRIEND_ATTACKED)){
        anger_adjust -= 15;
    }

    if (anger_adjust != 0 && morale_adjust != 0)
    {
        for (size_t i = 0; i < g->num_zombies(); i++)
        {
            g->zombie(i).morale += morale_adjust;
            g->zombie(i).anger += anger_adjust;
        }
    }
}

void monster::hit_monster(monster &other)
{
    // TODO: Unify this with the function above
    moves -= 100;

    if( this == &other ) {
        return;
    }

    damage_instance damage;
    if( !is_hallucination() ) {
        if( type->melee_dice > 0 ) {
            damage.add_damage( DT_BASH, dice( type->melee_dice, type->melee_sides ) );
        }

        if( type->melee_cut > 0 ) {
            damage.add_damage( DT_CUT, type->melee_cut );
        }
    }

    dealt_damage_instance dealt_dam;
    int hitspread = other.deal_melee_attack( this, hit_roll() );
    if( hitspread >= 0 ) {
        other.deal_melee_hit( this, hitspread, false, damage, dealt_dam );
        if( g->u.sees(*this) ) {
            add_msg(_("The %s hits the %s!"), name().c_str(), other.name().c_str());
        }
    } else {
        if( g->u.sees( *this ) ) {
            add_msg(_("The %s misses the %s!"), name().c_str(), other.name().c_str());
        }

        if( !is_hallucination() ) {
            other.on_dodge( this );
        }
    }
}

int monster::deal_melee_attack(Creature *source, int hitroll)
{
    int roll = Creature::deal_melee_attack(source, hitroll);
    if( is_hallucination() ) {
        return roll;
    }

    if( roll < 0 ) {
        // on_hit is in deal_melee_hit
        on_dodge( source );
    }

    return roll;
}

int monster::deal_projectile_attack(Creature *source, double missed_by,
                                    const projectile& proj, dealt_damage_instance &dealt_dam) {
    bool u_see_mon = g->u.sees(*this);
    // Maxes out at 50% chance with perfect hit
    if (has_flag(MF_HARDTOSHOOT) && !one_in(10 - 10 * (.8 - missed_by)) && !proj.wide) {
        if (u_see_mon) {
            add_msg(_("The shot passes through %s without hitting."), disp_name().c_str());
        }
        return 1;
    }
    // Not HARDTOSHOOT
    // if it's a headshot with no head, make it not a headshot
    if (missed_by < 0.2 && has_flag(MF_NOHEAD)) {
        missed_by = 0.2;
    }

    // whip has a chance to scare wildlife
    if(proj.proj_effects.count("WHIP") && type->in_category("WILDLIFE") && one_in(3)) {
        add_effect("run", rng(3, 5));
    }

    int went_past = Creature::deal_projectile_attack(source, missed_by, proj, dealt_dam);
    if( !is_hallucination() && !went_past ) {
        // Maybe TODO: Get difficulty from projectile speed/size/missed_by
        on_hit( source, bp_torso, INT_MIN, &proj );
    }

    return went_past;
}

void monster::deal_damage_handle_type(const damage_unit& du, body_part bp, int& damage, int& pain) {
    switch (du.type) {
    case DT_ELECTRIC:
        if (has_flag(MF_ELECTRIC)) {
            damage += 0; // immunity
            pain += 0;
            return; // returns, since we don't want a fallthrough
        }
        break;
    case DT_COLD:
        if (!has_flag(MF_WARM)) {
            damage += 0; // immunity
            pain += 0;
            return;
        }
        break;
    case DT_BASH:
        if (has_flag(MF_PLASTIC)) {
            damage += du.amount / rng(2,4); // lessened effect
            pain += du.amount / 4;
            return;
        }
        break;
    case DT_NULL:
        debugmsg("monster::deal_damage_handle_type: illegal damage type DT_NULL");
        break;
    case DT_ACID:
        if( has_flag( MF_ACIDPROOF ) ) {
            damage += 0; // immunity
            pain += 0;
            return;
        }
    case DT_TRUE: // typeless damage, should always go through
    case DT_BIOLOGICAL: // internal damage, like from smoke or poison
    case DT_CUT:
    case DT_STAB:
    case DT_HEAT:
    default:
        break;
    }

    Creature::deal_damage_handle_type(du, bp, damage, pain);
}

void monster::heal( const int delta_hp )
{
    hp += delta_hp;
}

void monster::set_hp( const int hp )
{
    this->hp = hp;
}

void monster::apply_damage(Creature* source, body_part /*bp*/, int dam) {
    if( is_dead_state() ) {
        return;
    }
    hp -= dam;
    if( hp < 1 ) {
        set_killer( source );
    } else if( dam > 0 ) {
        process_trigger( MTRIG_HURT, 1 + int( dam / 3 ) );
    }
}

void monster::die_in_explosion(Creature* source)
{
    hp = -9999; // huge to trigger explosion and prevent corpse item
    die( source );
}

bool monster::move_effects(bool attacking)
{
    bool u_see_me = g->u.sees(*this);
    if (has_effect("tied")) {
        return false;
    }
    if (has_effect("downed")) {
        remove_effect("downed");
        if (u_see_me) {
            add_msg(_("The %s climbs to its feet!"), name().c_str());
        }
        return false;
    }
    if (has_effect("webbed")) {
        if (x_in_y(type->melee_dice * type->melee_sides, 6 * get_effect_int("webbed"))) {
            if (u_see_me) {
                add_msg(_("The %s breaks free of the webs!"), name().c_str());
            }
            remove_effect("webbed");
        }
        return false;
    }
    if (has_effect("lightsnare")) {
        if(x_in_y(type->melee_dice * type->melee_sides, 12)) {
            remove_effect("lightsnare");
            g->m.spawn_item(posx(), posy(), "string_36");
            g->m.spawn_item(posx(), posy(), "snare_trigger");
            if (u_see_me) {
                add_msg(_("The %s escapes the light snare!"), name().c_str());
            }
        }
        return false;
    }
    if (has_effect("heavysnare")) {
        if (type->melee_dice * type->melee_sides >= 7) {
            if(x_in_y(type->melee_dice * type->melee_sides, 32)) {
                remove_effect("heavysnare");
                g->m.spawn_item(posx(), posy(), "rope_6");
                g->m.spawn_item(posx(), posy(), "snare_trigger");
                if (u_see_me) {
                    add_msg(_("The %s escapes the heavy snare!"), name().c_str());
                }
            }
        }
        return false;
    }
    if (has_effect("beartrap")) {
        if (type->melee_dice * type->melee_sides >= 18) {
            if(x_in_y(type->melee_dice * type->melee_sides, 200)) {
                remove_effect("beartrap");
                g->m.spawn_item(posx(), posy(), "beartrap");
                if (u_see_me) {
                    add_msg(_("The %s escapes the bear trap!"), name().c_str());
                }
            }
        }
        return false;
    }
    if (has_effect("crushed")) {
        // Strength helps in getting free, but dex also helps you worm your way out of the rubble
        if(x_in_y(type->melee_dice * type->melee_sides, 100)) {
            remove_effect("crushed");
            if (u_see_me) {
                add_msg(_("The %s frees itself from the rubble!"), name().c_str());
            }
        }
        return false;
    }

    // If we ever get more effects that force movement on success this will need to be reworked to
    // only trigger success effects if /all/ rolls succeed
    if (has_effect("in_pit")) {
        if (rng(0, 40) > type->melee_dice * type->melee_sides) {
            return false;
        } else {
            if (u_see_me) {
                add_msg(_("The %s escapes the pit!"), name().c_str());
            }
            remove_effect("in_pit");
        }
    }
    if (has_effect("grabbed")){
        if ( (dice(type->melee_dice + type->melee_sides, 3) < get_effect_int("grabbed")) || !one_in(4) ){
            return false;
        } else {
            if (u_see_me) {
                add_msg(_("The %s breaks free from the grab!"), name().c_str());
            }
            remove_effect("grabbed");
        }
    }
    return Creature::move_effects(attacking);
}

void monster::add_eff_effects(effect e, bool reduced)
{
    int val = e.get_amount("HURT", reduced);
    if (val > 0) {
        if(e.activated(calendar::turn, "HURT", val, reduced)) {
            apply_damage(nullptr, bp_torso, val);
        }
    }
    Creature::add_eff_effects(e, reduced);
}
void monster::add_effect( efftype_id eff_id, int dur, body_part bp,
                          bool permanent, int intensity, bool force )
{
    bp = num_bp;
    Creature::add_effect( eff_id, dur, bp, permanent, intensity, force );
}

int monster::get_armor_cut(body_part bp) const
{
    (void) bp;
    // TODO: Add support for worn armor?
    return int(type->armor_cut) + armor_bash_bonus;
}

int monster::get_armor_bash(body_part bp) const
{
    (void) bp;
    return int(type->armor_bash) + armor_cut_bonus;
}

int monster::hit_roll() const {
    //Unstable ground chance of failure
    if (has_effect("bouldering")) {
        if(one_in(type->melee_skill)) {
            return 0;
        }
    }

    return dice(type->melee_skill, 10);
}

bool monster::has_grab_break_tec() const
{
    return false;
}

int monster::stability_roll() const
{
    int size_bonus = 0;
    switch (type->size) {
        case MS_TINY:
            size_bonus -= 7;
            break;
        case MS_SMALL:
            size_bonus -= 3;
            break;
        case MS_LARGE:
            size_bonus += 5;
            break;
        case MS_HUGE:
            size_bonus += 10;
            break;
        case MS_MEDIUM:
            break; // keep default
    }

    int stability = dice(type->melee_sides, type->melee_dice) + size_bonus;
    if( has_effect( "stunned" ) ) {
        stability -= rng( 1, 5 );
    }
    return stability;
}

int monster::get_dodge() const
{
    if (has_effect("downed")) {
        return 0;
    }
    int ret = type->sk_dodge;
    if (has_effect("lightsnare") || has_effect("heavysnare") || has_effect("beartrap") || has_effect("tied")) {
        ret /= 2;
    }
    if (moves <= 0 - 100 - get_speed()) {
        ret = rng(0, ret);
    }
    return ret + get_dodge_bonus();
}

int monster::get_melee() const
{
    return type->melee_skill;
}

int monster::dodge_roll()
{
    if (has_effect("bouldering")) {
        if(one_in(type->sk_dodge)) {
            return 0;
        }
    }

    int numdice = get_dodge();

    switch (type->size) {
        case MS_TINY:
            numdice += 6;
            break;
        case MS_SMALL:
            numdice += 3;
            break;
        case MS_LARGE:
            numdice -= 2;
            break;
        case MS_HUGE:
            numdice -= 4;
            break;
        case MS_MEDIUM:
            break; // keep default
    }

    numdice += get_speed() / 80;
    return dice(numdice, 10);
}

float monster::fall_damage_mod() const
{
    if( has_flag(MF_FLIES) ) {
        return 0.0f;
    }

    switch (type->size) {
        case MS_TINY:
            return 0.2f;
        case MS_SMALL:
            return 0.6f;
        case MS_MEDIUM:
            return 1.0f;
        case MS_LARGE:
            return 1.4f;
        case MS_HUGE:
            return 2.0f;
    }

    return 0.0f;
}

int monster::impact( const int force, const tripoint &p )
{
    if( force <= 0 ) {
        return force;
    }

    const float mod = fall_damage_mod();
    int total_dealt = 0;
    if( g->m.has_flag( TFLAG_SHARP, p ) ) {
        const int cut_damage = 10 * mod - get_armor_cut( bp_torso );
        apply_damage( nullptr, bp_torso, cut_damage );
        total_dealt += 10 * mod;
    }

    const int bash_damage = force * mod - get_armor_bash( bp_torso );
    apply_damage( nullptr, bp_torso, bash_damage );
    total_dealt += force * mod;

    add_effect( "downed", rng( 0, mod * 3 + 1 ) );

    return total_dealt;
}

void monster::reset_special(int index)
{
    if (index < 0) {
        return;
    }

    sp_timeout[index] = type->sp_freq[index];
}

void monster::reset_special_rng(int index)
{
    if (index < 0) {
        return;
    }

    sp_timeout[index] = rng(0, type->sp_freq[index]);
}

void monster::set_special(int index, int time)
{
    if (index < 0) {
        return;
    }

    // -1 is used for disabling specials
    if (time < -1) {
        time = 0;
    }
    sp_timeout[index] = time;
}

void monster::normalize_ammo( const int old_ammo )
{
    int total_ammo = 0;
    // Sum up the ammo entries to get a ratio.
    for( const auto &ammo_entry : type->starting_ammo ) {
        total_ammo += ammo_entry.second;
    }
    if( total_ammo == 0 ) {
        // Should never happen, but protect us from a div/0 if it does.
        return;
    }
    // Previous code gave robots 100 rounds of ammo.
    // This reassigns whatever is left from that in the appropriate proportions.
    for( const auto &ammo_entry : type->starting_ammo ) {
        ammo[ammo_entry.first] = (old_ammo * ammo_entry.second) / (100 * total_ammo);
    }
}

void monster::explode()
{
    if( is_hallucination() ) {
        //Can't gib hallucinations
        return;
    }
    if( type->has_flag( MF_NOGIB ) || type->has_flag( MF_VERMIN ) ) {
        return;
    }
    // Send body parts and blood all over!
    const itype_id meat = type->get_meat_itype();
    const field_id type_blood = bloodType();
    const field_id type_gib = gibType();
    if( meat != "null" || type_blood != fd_null || type_gib != fd_null ) {
        // Only create chunks if we know what kind to make.
        int num_chunks = 0;
        switch( type->size ) {
            case MS_TINY:
                num_chunks = 1;
                break;
            case MS_SMALL:
                num_chunks = 2;
                break;
            case MS_MEDIUM:
                num_chunks = 4;
                break;
            case MS_LARGE:
                num_chunks = 8;
                break;
            case MS_HUGE:
                num_chunks = 16;
                break;
        }

        for( int i = 0; i < num_chunks; i++ ) {
            tripoint tarp( posx() + rng( -3, 3 ), posy() + rng( -3, 3 ), posz() );
            std::vector<tripoint> traj = line_to( pos(), tarp, 0, 0 );

            for( size_t j = 0; j < traj.size(); j++ ) {
                tarp = traj[j];
                if( one_in( 2 ) && type_blood != fd_null ) {
                    g->m.add_field( tarp, type_blood, 1, 0 );
                } else if( type_gib != fd_null ) {
                    g->m.add_field( tarp, type_gib, rng( 1, j + 1 ), 0 );
                }
                if( g->m.move_cost( tarp ) == 0 ) {
                    if( !g->m.bash( tarp, 3 ).second ) {
                        // Target is obstacle, not destroyed by bashing,
                        // stop trajectory in front of it, if this is the first
                        // point (e.g. wall adjacent to monster) , make it invalid.
                        if( j > 0 ) {
                            tarp = traj[j - 1];
                        } else {
                            tarp = tripoint_min;
                        }
                        break;
                    }
                }
            }
            if( meat != "null" && tarp != tripoint_min ) {
                g->m.spawn_item( tarp, meat, 1, 0, calendar::turn );
            }
        }
    }
}

void monster::die(Creature* nkiller) {
    if( dead ) {
        // We are already dead, don't die again, note that monster::dead is
        // *only* set to true in this function!
        return;
    }
    dead = true;
    set_killer( nkiller );
    if( hp < -( type->size < MS_MEDIUM ? 1.5 : 3 ) * type->hp ) {
        explode(); // Explode them if it was big overkill
    }
    if (!no_extra_death_drops) {
        drop_items_on_death();
    }
    // TODO: should actually be class Character
    player *ch = dynamic_cast<player*>( get_killer() );
    if( !is_hallucination() && ch != nullptr ) {
        if( has_flag( MF_GUILT ) || ( ch->has_trait( "PACIFIST" ) && has_flag( MF_HUMAN ) ) ) {
            // has guilt flag or player is pacifist && monster is humanoid
            mdeath::guilt(this);
        }
        // TODO: add a kill counter to npcs?
        if( ch->is_player() ) {
            g->increase_kill_count( type->id );
        }
        if( type->difficulty >= 30 ) {
            ch->add_memorial_log( pgettext( "memorial_male", "Killed a %s." ),
                                  pgettext( "memorial_female", "Killed a %s." ),
                                  name().c_str() );
        }
    }
    // We were tied up at the moment of death, add a short rope to inventory
    if ( has_effect("tied") ) {
        item rope_6("rope_6", 0);
        add_item(rope_6);
    }
    if( has_effect( "lightsnare" ) ) {
        add_item( item( "string_36", 0 ) );
        add_item( item( "snare_trigger", 0 ) );
    }
    if( has_effect( "heavysnare" ) ) {
        add_item( item( "rope_6", 0 ) );
        add_item( item( "snare_trigger", 0 ) );
    }
    if( has_effect( "beartrap" ) ) {
        add_item( item( "beartrap", 0 ) );
    }

    if( !is_hallucination() ) {
        for( const auto &it : inv ) {
            g->m.add_item_or_charges( posx(), posy(), it );
        }
    }

    // If we're a queen, make nearby groups of our type start to die out
    if( !is_hallucination() && has_flag( MF_QUEEN ) ) {
        // The submap coordinates of this monster, monster groups coordinates are
        // submap coordinates.
        const point abssub = overmapbuffer::ms_to_sm_copy( g->m.getabs( posx(), posy() ) );
        // Do it for overmap above/below too
        for( int z = 1; z >= -1; --z ) {
            for( int x = -MAPSIZE / 2; x <= MAPSIZE / 2; x++ ) {
                for( int y = -MAPSIZE / 2; y <= MAPSIZE / 2; y++ ) {
                    std::vector<mongroup*> groups = overmap_buffer.groups_at( abssub.x + x, abssub.y + y, g->get_levz() + z );
                    for( auto &mgp : groups ) {
                        if( MonsterGroupManager::IsMonsterInGroup( mgp->type, type->id ) ) {
                            mgp->dying = true;
                        }
                    }
                }
            }
        }
    }
    mission::on_creature_death( *this );
    // Also, perform our death function
    if(is_hallucination()) {
        //Hallucinations always just disappear
        mdeath::disappear(this);
        return;
    }

    //Not a hallucination, go process the death effects.
    for (auto const &deathfunction : type->dies) {
        deathfunction(this);
    }

    // If our species fears seeing one of our own die, process that
    int anger_adjust = 0, morale_adjust = 0;
    if( type->has_anger_trigger( MTRIG_FRIEND_DIED ) ) {
        anger_adjust += 15;
    }
    if( type->has_fear_trigger( MTRIG_FRIEND_DIED ) ) {
        morale_adjust -= 15;
    }
    if( type->has_placate_trigger( MTRIG_FRIEND_DIED ) ) {
        anger_adjust -= 15;
    }

    if (anger_adjust != 0 || morale_adjust != 0) {
        int light = g->light_level();
        for (size_t i = 0; i < g->num_zombies(); i++) {
            monster &critter = g->zombie( i );
            if( !critter.type->same_species( *type ) ) {
                continue;
            }

            if( g->m.sees( critter.pos3(), pos3(), light ) ) {
                critter.morale += morale_adjust;
                critter.anger += anger_adjust;
            }
        }
    }
}

void monster::drop_items_on_death()
{
    if(is_hallucination()) {
        return;
    }
    if (type->death_drops.empty()) {
        return;
    }
    g->m.put_items_from_loc( type->death_drops, pos3(), calendar::turn );
}

void monster::process_effects()
{
    // Monster only effects
    int mod = 1;
    for( auto &elem : effects ) {
        for( auto &_effect_it : elem.second ) {
            auto &it = _effect_it.second;
            // Monsters don't get trait-based reduction, but they do get effect based reduction
            bool reduced = has_effect(it.get_resist_effect());

            mod_speed_bonus(it.get_mod("SPEED", reduced));

            int val = it.get_mod("HURT", reduced);
            if (val > 0) {
                if(it.activated(calendar::turn, "HURT", val, reduced, mod)) {
                    apply_damage(nullptr, bp_torso, val);
                }
            }

            std::string id = _effect_it.second.get_id();
            // MATERIALS-TODO: use fire resistance
            if (id == "onfire") {
                if (made_of("flesh") || made_of("iflesh"))
                    apply_damage( nullptr, bp_torso, rng( 3, 8 ) );
                if (made_of("veggy"))
                    apply_damage( nullptr, bp_torso, rng( 10, 20 ) );
                if (made_of("paper") || made_of("powder") || made_of("wood") || made_of("cotton") ||
                    made_of("wool"))
                    apply_damage( nullptr, bp_torso, rng( 15, 40 ) );
            }
        }
    }

    //If this monster has the ability to heal in combat, do it now.
    if( has_flag( MF_REGENERATES_50 ) ) {
        if( hp < type->hp ) {
            if( one_in( 2 ) && g->u.sees( *this ) ) {
                add_msg( m_warning, _( "The %s is visibly regenerating!" ), name().c_str() );
            }
            hp += 50;
            if( hp > type->hp ) {
                hp = type->hp;
            }
        }
    }
    if( has_flag( MF_REGENERATES_10 ) ) {
        if( hp < type->hp ) {
            if( one_in( 2 ) && g->u.sees( *this ) ) {
                add_msg( m_warning, _( "The %s seems a little healthier." ), name().c_str() );
            }
            hp += 10;
            if( hp > type->hp ) {
                hp = type->hp;
            }
        }
    }

    //Monster will regen morale and aggression if it is on max HP
    //It regens more morale and aggression if is currently fleeing.
    if( has_flag( MF_REGENMORALE ) && hp >= type->hp ) {
        if( is_fleeing( g->u ) ) {
            morale = type->morale;
            anger = type->agro;
        }
        if( morale <= type->morale ) {
            morale += 1;
        }
        if( anger <= type->agro ) {
            anger += 1;
        }
        if( morale < 0 ) {
            morale += 5;
        }
        if( anger < 0 ) {
            anger += 5;
        }
    }

    // If this critter dies in sunlight, check & assess damage.
    if( has_flag( MF_SUNDEATH ) && g->is_in_sunlight( pos() ) ) {
        if( g->u.sees( *this ) ) {
            add_msg( m_good, _( "The %s burns horribly in the sunlight!" ), name().c_str() );
        }
        apply_damage( nullptr, bp_torso, 100 );
        if( hp < 0 ) {
            hp = 0;
        }
    }

    Creature::process_effects();
}

bool monster::make_fungus()
{
    if( is_hallucination() ) {
        return true;
    }
    char polypick = 0;
    std::string tid = type->id;
    if (type->in_species("FUNGUS")) { // No friendly-fungalizing ;-)
        return true;
    }
    if (tid == "mon_ant" || tid == "mon_ant_soldier" || tid == "mon_ant_queen" || tid == "mon_fly" ||
      tid == "mon_bee" || tid == "mon_dermatik") {
        polypick = 1;
    } else if (tid == "mon_zombie" || tid == "mon_zombie_shrieker" || tid == "mon_zombie_electric" ||
      tid == "mon_zombie_spitter" || tid == "mon_zombie_dog" || tid == "mon_zombie_brute" ||
      tid == "mon_zombie_hulk" || tid == "mon_zombie_soldier" || tid == "mon_zombie_tough" ||
      tid == "mon_zombie_scientist" || tid == "mon_zombie_hunter" || tid == "mon_zombie_child"||
      tid == "mon_zombie_bio_op" || tid == "mon_zombie_survivor" || tid == "mon_zombie_fireman" ||
      tid == "mon_zombie_cop" || tid == "mon_zombie_fat" || tid == "mon_zombie_rot" ||
      tid == "mon_zombie_swimmer" || tid == "mon_zombie_grabber" || tid == "mon_zombie_technician" ||
      tid == "mon_zombie_brute_shocker" || tid == "mon_zombie_grenadier" ||
      tid == "mon_zombie_grenadier_elite") {
        polypick = 2; // Necro and Master have enough Goo to resist conversion.
        // Firefighter, hazmat, and scarred/beekeeper have the PPG on.
    } else if (tid == "mon_zombie_necro" || tid == "mon_zombie_master" || tid == "mon_zombie_fireman" ||
      tid == "mon_zombie_hazmat" || tid == "mon_beekeeper") {
        return true;
    } else if (tid == "mon_boomer" || tid == "mon_zombie_gasbag" || tid == "mon_zombie_smoker") {
        polypick = 3;
    } else if (tid == "mon_triffid" || tid == "mon_triffid_young" || tid == "mon_triffid_queen") {
        polypick = 4;
    }
    switch (polypick) {
        case 1: // bugs, why do they all turn into fungal ants?
            poly(GetMType("mon_ant_fungus"));
            return true;
        case 2: // zombies, non-boomer
            poly(GetMType("mon_zombie_fungus"));
            return true;
        case 3:
            poly(GetMType("mon_boomer_fungus"));
            return true;
        case 4:
            poly(GetMType("mon_fungaloid"));
            return true;
        default:
            return false;
    }
}

void monster::make_friendly()
{
 plans.clear();
 friendly = rng(5, 30) + rng(0, 20);
}

void monster::make_ally(monster *z) {
    friendly = z->friendly;
    faction = z->faction;
}

int monster::get_last_load() const
{
    return last_loaded;
}

void monster::set_last_load(int day)
{
    last_loaded = day;
}

void monster::reset_last_load()
{
    last_loaded = calendar::turn.get_turn() / DAYS(1);
}

void monster::add_item(item it)
{
 inv.push_back(it);
}

bool monster::is_hallucination() const
{
  return hallucination;
}

field_id monster::bloodType() const {
    return type->bloodType();
}
field_id monster::gibType() const {
    return type->gibType();
}

m_size monster::get_size() const {
    return type->size;
}


void monster::add_msg_if_npc(const char *msg, ...) const
{
    va_list ap;
    va_start(ap, msg);
    std::string processed_npc_string = vstring_format(msg, ap);
    processed_npc_string = replace_with_npc_name(processed_npc_string, disp_name());
    add_msg(processed_npc_string.c_str());
    va_end(ap);
}

void monster::add_msg_player_or_npc(const char *, const char* npc_str, ...) const
{
    va_list ap;
    va_start(ap, npc_str);
    if (g->u.sees(*this)) {
        std::string processed_npc_string = vstring_format(npc_str, ap);
        processed_npc_string = replace_with_npc_name(processed_npc_string, disp_name());
        add_msg(processed_npc_string.c_str());
    }
    va_end(ap);
}

void monster::add_msg_if_npc(game_message_type type, const char *msg, ...) const
{
    va_list ap;
    va_start(ap, msg);
    std::string processed_npc_string = vstring_format(msg, ap);
    processed_npc_string = replace_with_npc_name(processed_npc_string, disp_name());
    add_msg(type, processed_npc_string.c_str());
    va_end(ap);
}

void monster::add_msg_player_or_npc(game_message_type type, const char *, const char* npc_str, ...) const
{
    va_list ap;
    va_start(ap, npc_str);
    if (g->u.sees(*this)) {
        std::string processed_npc_string = vstring_format(npc_str, ap);
        processed_npc_string = replace_with_npc_name(processed_npc_string, disp_name());
        add_msg(type, processed_npc_string.c_str());
    }
    va_end(ap);
}

bool monster::is_dead() const
{
    return dead || is_dead_state();
}

void monster::init_from_item( const item &itm )
{
    if( itm.typeId() == "corpse" ) {
        const int burnt_penalty = itm.burnt;
        set_speed_base( static_cast<int>( get_speed_base() * 0.8 ) - ( burnt_penalty / 2 ) );
        hp = static_cast<int>( hp * 0.7 ) - burnt_penalty;
        if( itm.damage > 0 ) {
            set_speed_base( speed_base / ( itm.damage + 1 ) );
            hp /= itm.damage + 1;
        }
        hp = std::max( 1, hp ); // Otherwise burned monsters will rez with <= 0 hp
    } else {
        // must be a robot
        const int damfac = 5 - std::max<int>( 0, itm.damage ); // 5 (no damage) ... 1 (max damage)
        // One hp at least, everything else would be unfair (happens only to monster with *very* low hp),
        hp = std::max( 1, hp * damfac / 5 );
    }
}

item monster::to_item() const
{
    if( type->revert_to_itype.empty() ) {
        return item();
    }
    // Birthday is wrong, but the item created here does not use it anyway (I hope).
    item result( type->revert_to_itype, calendar::turn );
    const int damfac = std::max( 1, 5 * hp / type->hp ); // 1 ... 5 (or more for some monsters with hp > type->hp)
    result.damage = std::max( 0, 5 - damfac ); // 4 ... 0
    return result;
}

float monster::power_rating() const
{
    float ret = get_size() - 1; // Zed gets 1, cat -1, hulk 3
    ret += has_flag( MF_ELECTRONIC ) ? 2 : 0; // Robots tend to have guns
    // Hostile stuff gets a big boost
    // Neutral moose will still get burned if it comes close
    return ret;
}

void monster::on_dodge( Creature*, int )
{
    // Currently does nothing, later should handle faction relations
}

void monster::on_hit( Creature *source, body_part,
                      int, projectile const* const proj )
{
    type->sp_defense( this, source, proj );
    check_dead_state();
    // TODO: Faction relations
}

body_part monster::get_random_body_part( bool ) const
{
    return bp_torso;
}

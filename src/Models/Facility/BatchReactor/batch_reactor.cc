// batch_reactor.cc
// Implements the BatchReactor class
#include "batch_reactor.h"

#include <sstream>
#include <cmath>

#include <boost/lexical_cast.hpp>

#include "logger.h"
#include "generic_resource.h"
#include "recipe_library.h"
#include "market_model.h"
#include "cyc_limits.h"
#include "error.h"

using namespace std;
using boost::lexical_cast;

// static members
map<Phase,string> BatchReactor::phase_names_ = map<Phase,string>();

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BatchReactor::BatchReactor() :
  cycle_length_(1),
  refuel_delay_(0),
  batches_per_core_(1),
  in_core_loading_(1),
  out_core_loading_(1),
  in_commodity_(""),
  in_recipe_(""),
  out_commodity_(""),
  out_recipe_(""),
  cycle_timer_(0),
  phase_(INIT)
{
  preCore_.SetCapacity(cyclus::kBuffInfinity);
  inCore_.SetCapacity(cyclus::kBuffInfinity);
  postCore_.SetCapacity(cyclus::kBuffInfinity);
  if (phase_names_.size() < 1)
    setUpPhaseNames();
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BatchReactor::~BatchReactor() {}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::InitModuleMembers(cyclus::QueryEngine* qe)
{
  cyclus::QueryEngine* input = qe->QueryElement("fuel_input");
  set_in_commodity(input->GetElementContent("incommodity"));
  set_in_recipe(input->GetElementContent("inrecipe"));

  cyclus::QueryEngine* output = qe->QueryElement("fuel_output");
  set_out_commodity(output->GetElementContent("outcommodity"));
  set_out_recipe(output->GetElementContent("outrecipe"));

  string data;
  data = qe->GetElementContent("cyclelength");
  set_cycle_length(lexical_cast<int>(data));

  try
    {
      data = qe->GetElementContent("refueldelay");
      set_refuel_delay(lexical_cast<double>(data));
    }
  catch (cyclus::Error e) {}

  data = qe->GetElementContent("incoreloading");
  set_in_core_loading(lexical_cast<double>(data));

  try
    {
      data = qe->GetElementContent("outcoreloading");
      set_out_core_loading(lexical_cast<double>(data));
    }
  catch (cyclus::Error e)
    {
      set_out_core_loading(in_core_loading());
    }

  data = qe->GetElementContent("batchespercore");
  set_batches_per_core(lexical_cast<int>(data));

  cyclus::QueryEngine* commodity = qe->QueryElement("commodity_production");
  cyclus::Commodity commod(commodity->GetElementContent("commodity"));
  AddCommodity(commod);
  data = commodity->GetElementContent("capacity");
  cyclus::supply_demand::CommodityProducer::SetCapacity(commod,lexical_cast<double>(data));
  data = commodity->GetElementContent("cost");
  cyclus::supply_demand::CommodityProducer::SetCost(commod,lexical_cast<double>(data));
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
std::string BatchReactor::str()
{
  std::stringstream ss;
  ss << cyclus::FacilityModel::str();
  ss << " has facility parameters {"
     << ", Cycle Length = " << cycle_length()
     << ", Refuel Delay = " << refuel_delay()
     << ", InCore Loading = " << in_core_loading()
     << ", OutCore Loading = " << out_core_loading()
     << ", Batches Per Core = " << batches_per_core()
     << ", converts commodity '" << in_commodity()
     << "' into commodity '" << out_commodity()
     << "'}";
  return ss.str();
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::CloneModuleMembersFrom(cyclus::FacilityModel* sourceModel)
{
  BatchReactor* source = dynamic_cast<BatchReactor*>(sourceModel);
  set_cycle_length(source->cycle_length());
  set_refuel_delay(source->refuel_delay());
  set_in_core_loading(source->in_core_loading());
  set_out_core_loading(source->out_core_loading());
  set_batches_per_core(source->batches_per_core());
  set_in_commodity(source->in_commodity());
  set_out_commodity(source->out_commodity());
  set_in_recipe(source->in_recipe());
  set_out_recipe(source->out_recipe());
  CopyProducedCommoditiesFrom(source);
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::EnterSimulationAsModule()
{
  preCore_.SetCapacity(in_core_loading());
  inCore_.SetCapacity(in_core_loading());
  reset_cycle_timer();
  setPhase(BEGIN);
  LOG(cyclus::LEV_DEBUG2,"BReact") << "Batch Reactor " << name()
                           << " is entering the simuluation with members:";
  LOG(cyclus::LEV_DEBUG2,"BReact") << "  * in core loading: " << in_core_loading();
  LOG(cyclus::LEV_DEBUG2,"BReact") << "  * out core loading: " << out_core_loading();
  LOG(cyclus::LEV_DEBUG2,"BReact") << "  * pre core capacity: " << preCore_.capacity();
  LOG(cyclus::LEV_DEBUG2,"BReact") << "  * in core capacity: " << inCore_.capacity();
  LOG(cyclus::LEV_DEBUG2,"BReact") << "  * cycle timer: " << cycle_timer_;
  LOG(cyclus::LEV_DEBUG2,"BReact") << "  * phase: " << phase_names_[phase_];
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::HandleTick(int time)
{
  LOG(cyclus::LEV_INFO3, "BReact") << name() << " is ticking at time "
                           << time << " {";
  LOG(cyclus::LEV_DEBUG3, "BReact") << "The current phase is: "
                            << phase_names_[phase_];


  if ( LifetimeReached(time) )
    setPhase(END);

  double fuel_quantity, request;
  string msg;

  switch( phase() )
    {
    case INIT:
      // intentional fall through

    case OPERATION:
      cycle_timer_++;
      break;

    case REFUEL:
      offloadBatch();

    case REFUEL_DELAY:
      // intentional fall through

    case WAITING:
      // intentional fall through

    case BEGIN:
      cycle_timer_++;
      fuel_quantity = preCore_.quantity() + inCore_.quantity();
      request = in_core_loading() - fuel_quantity;
      if (request > cyclus::eps())
        makeRequest(request);
      break;

    case END:
      offloadCore();
      break;

    default:
      msg = "BatchReactors have undefined behvaior during ticks for phase: "
        + phase_names_[phase_];
      throw cyclus::Error(msg);
      break;
    }

  makeOffers();

  LOG(cyclus::LEV_INFO3, "BReact") << "}";
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::HandleTock(int time)
{
  LOG(cyclus::LEV_INFO3, "BReact") << name() << " is tocking {";
  LOG(cyclus::LEV_DEBUG3, "BReact") << "The current phase is: "
                            << phase_names_[phase_];

  handleOrders();

  string msg;

  switch( phase() )
    {

    case END:
      // if ( postCore_.empty() )
      //   decommission();
      break;

    case BEGIN:
      // intentional fall through

    case WAITING:
      loadCore();
      if ( coreFilled() )
        {

          setPhase(OPERATION);
          reset_cycle_timer();
        }
      else
        {
          setPhase(WAITING);
        }
      break;

    case REFUEL:
      setPhase(REFUEL_DELAY);
      time_delayed_ = 0;
    case REFUEL_DELAY:
      loadCore();
      if ( time_delayed_ > refuel_delay() && coreFilled() )
        {
          setPhase(OPERATION);
          reset_cycle_timer();
        }
      else
        {
          ++time_delayed_;
        }
      break;

    case OPERATION:
      if (cycleComplete())
        setPhase(REFUEL);
      break;

    default:
      msg = "BatchReactors have undefined behvaior during tocks for phase: "
        + phase_names_[phase_];
      throw cyclus::Error(msg);
      break;
    }

  LOG(cyclus::LEV_DEBUG3, "BReact") << "cycle timer: "
                            << cycle_timer_;
  LOG(cyclus::LEV_DEBUG3, "BReact") << "delay: "
                            << time_delayed_;
  LOG(cyclus::LEV_INFO3, "BReact") << "}";
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::ReceiveMessage(cyclus::Message::Ptr msg)
{
  // is this a message from on high?
  if(msg->trans().supplier()==this)
    {
      // file the order
      ordersWaiting_.push_front(msg);
      LOG(cyclus::LEV_INFO5, "BReact") << name() << " just received an order.";
    }
  else
    {
      throw cyclus::Error("BatchReactor is not the supplier of this msg.");
    }
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::sendMessage(Communicator* recipient, cyclus::Transaction trans)
{
      cyclus::Message::Ptr msg(new cyclus::Message(this, recipient, trans));
      msg->SendOn();
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
vector<cyclus::Resource::Ptr> BatchReactor::removeResource(cyclus::Transaction order)
{
  cyclus::Transaction trans = order;
  double amt = trans.resource()->quantity();

  LOG(cyclus::LEV_DEBUG2, "BReact") << "BatchReactor " << name() << " removed "
                            << amt << " of " << postCore_.quantity()
                            << " to its postcore buffer.";

  return cyclus::MatBuff::ToRes(postCore_.PopQty(amt));
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::addResource(cyclus::Transaction trans,
                               std::vector<cyclus::Resource::Ptr> manifest)
{
  double preQuantity = preCore_.quantity();
  preCore_.PushAll(cyclus::MatBuff::ToMat(manifest));
  double added = preCore_.quantity() - preQuantity;
  LOG(cyclus::LEV_DEBUG2, "BReact") << "BatchReactor " << name() << " added "
                            << added << " to its precore buffer.";
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_cycle_length(int time)
{
  cycle_length_ = time;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int BatchReactor::cycle_length()
{
  return cycle_length_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_refuel_delay(int time)
{
  refuel_delay_ = time;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int BatchReactor::refuel_delay()
{
  return refuel_delay_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_in_core_loading(double size)
{
  in_core_loading_ = size;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
double BatchReactor::in_core_loading()
{
  return in_core_loading_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_out_core_loading(double size)
{
  out_core_loading_ = size;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
double BatchReactor::out_core_loading()
{
  return out_core_loading_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_batches_per_core(int n)
{
  batches_per_core_ = n;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int BatchReactor::batches_per_core()
{
  return batches_per_core_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
double BatchReactor::batchLoading()
{
  return in_core_loading_ / batches_per_core_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_in_commodity(std::string name)
{
  in_commodity_ = name;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
std::string BatchReactor::in_commodity()
{
  return in_commodity_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_in_recipe(std::string name)
{
  in_recipe_ = name;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
std::string BatchReactor::in_recipe()
{
  return in_recipe_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_out_commodity(std::string name)
{
  out_commodity_ = name;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
std::string BatchReactor::out_commodity()
{
  return out_commodity_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::set_out_recipe(std::string name)
{
  out_recipe_ = name;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
std::string BatchReactor::out_recipe()
{
  return out_recipe_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Phase BatchReactor::phase()
{
  return phase_;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BatchReactor::checkDecommissionCondition()
{
  bool empty = (preCore_.empty() && inCore_.empty() &&
                postCore_.empty());
  // if (!empty) {
  //   string msg = "Can't delete a BatchReactor with material still in its inventory.";
  //   throw cyclus::CycBatchReactorDestructException(msg);
  // }
  return empty;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::setPhase(Phase p)
{
  LOG(cyclus::LEV_DEBUG2, "BReact") << "BatchReactor " << name()
                            << " is changing phases -";
  LOG(cyclus::LEV_DEBUG2, "BReact") << "  * from phase: " << phase_names_[phase_];
  LOG(cyclus::LEV_DEBUG2, "BReact") << "  * to phase: " << phase_names_[p];
  phase_ = p;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::setUpPhaseNames()
{
  phase_names_.insert(make_pair(INIT,"initialization"));
  phase_names_.insert(make_pair(BEGIN,"beginning"));
  phase_names_.insert(make_pair(OPERATION,"operation"));
  phase_names_.insert(make_pair(REFUEL,"refueling"));
  phase_names_.insert(make_pair(REFUEL_DELAY,"refueling with delay"));
  phase_names_.insert(make_pair(WAITING,"waiting for fuel"));
  phase_names_.insert(make_pair(END,"ending"));
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::reset_cycle_timer()
{
  cycle_timer_ = 1;
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BatchReactor::cycleComplete()
{
  return (cycle_timer_ >= cycle_length_ - 1);
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BatchReactor::coreFilled()
{
  LOG(cyclus::LEV_DEBUG2,"BReact") << "Querying whether the core is filled -";
  LOG(cyclus::LEV_DEBUG2,"BReact") << "  * quantity in core: " << inCore_.quantity();
  LOG(cyclus::LEV_DEBUG2,"BReact") << "  * core capacity: " << inCore_.capacity();
  // @MJGFlag need to assert that the in core capacity must be > 0
  // 9/29/12 error with a negative in core capacity
  return (abs(inCore_.quantity() - inCore_.capacity()) < cyclus::eps());
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::makeRequest(double amt)
{
  interactWithMarket(in_commodity(),amt,cyclus::REQUEST);
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::makeOffers()
{
  if (!postCore_.empty())
    interactWithMarket(out_commodity(),postCore_.quantity(),cyclus::OFFER);
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::interactWithMarket(std::string commod, double amt, cyclus::TransType type)
{
  LOG(cyclus::LEV_INFO4, "BReact") << " making requests {";
  // get the market
  cyclus::MarketModel* market = cyclus::MarketModel::MarketForCommod(commod);
  Communicator* recipient = dynamic_cast<Communicator*>(market);
  // set the price
  double commodity_price = 0;
  // request a generic resource
  // build the transaction and message
  cyclus::Transaction trans(this, type);
  trans.SetCommod(commod);
  trans.SetMinFrac(1.0);
  trans.SetPrice(commodity_price);

  if (type == cyclus::OFFER)
    {
      cyclus::GenericResource::Ptr trade_res = cyclus::GenericResource::Ptr(new cyclus::GenericResource( "kg",commod, amt));
      trans.SetResource(trade_res);
    }
  else
    {
      cyclus::Material::Ptr trade_res = cyclus::Material::Ptr(new cyclus::Material(cyclus::RecipeLibrary::Recipe(in_recipe_)));
      trade_res->SetQuantity(amt);
      trans.SetResource(trade_res);

      LOG(cyclus::LEV_DEBUG1, "BatR") << "Requesting material: ";
      trade_res->Print();
    }

  // log the event
  string text;
  if (type == cyclus::OFFER)
    {
      text = " has offered ";
    }
  else
    {
      text = " has requested ";
    }
  LOG(cyclus::LEV_INFO5, "BReact") << name() << text << amt
                           << " kg of " << commod << ".";
  // send the message
  sendMessage(recipient, trans);
  LOG(cyclus::LEV_INFO4, "BReact") << "}";
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::handleOrders()
{
  while(!ordersWaiting_.empty())
    {
    cyclus::Message::Ptr order = ordersWaiting_.front();
    order->trans().ApproveTransfer();
    ordersWaiting_.pop_front();
    }
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::moveFuel(cyclus::MatBuff& fromBuff, cyclus::MatBuff& toBuff, double amt)
{
  vector<cyclus::Material::Ptr> to_move = fromBuff.PopQty(amt);
  for (int i = 0; i < to_move.size(); i++)
    {
      toBuff.PushOne(to_move.at(i));
    }
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::offLoadFuel(double amt)
{
  inCore_.PopQty(amt);
  double factor = out_core_loading() / in_core_loading();
  double out_amount = amt * factor;
  cyclus::Material::Ptr out_fuel = cyclus::Material::Ptr(new cyclus::Material(cyclus::RecipeLibrary::Recipe(out_recipe())));
  out_fuel->SetQuantity(out_amount);
  postCore_.PushOne(out_fuel);
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::loadCore()
{
  if (preCore_.quantity() > cyclus::eps())
    {
      moveFuel(preCore_,inCore_,preCore_.quantity());
      LOG(cyclus::LEV_DEBUG2, "BReact") << "BatchReactor " << name()
                                << " moved fuel into the core:";
      LOG(cyclus::LEV_DEBUG2, "BReact") << "  precore level: " << preCore_.quantity();
      LOG(cyclus::LEV_DEBUG2, "BReact") << "  incore level: " << inCore_.quantity();
    }
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::offloadBatch()
{
  offLoadFuel(batchLoading());

  LOG(cyclus::LEV_DEBUG2, "BReact") << "BatchReactor " << name()
                            << " removed a batch of fuel from the core:";
  LOG(cyclus::LEV_DEBUG2, "BReact") << "  incore level: " << inCore_.quantity();
  LOG(cyclus::LEV_DEBUG2, "BReact") << "  postcore level: " << postCore_.quantity();
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BatchReactor::offloadCore()
{
  offLoadFuel(inCore_.quantity());
  LOG(cyclus::LEV_DEBUG2, "BReact") << "BatchReactor " << name()
                            << " removed a core of fuel from the core:";
  LOG(cyclus::LEV_DEBUG2, "BReact") << "  precore level: " << preCore_.quantity();
  LOG(cyclus::LEV_DEBUG2, "BReact") << "  incore level: " << inCore_.quantity();
  LOG(cyclus::LEV_DEBUG2, "BReact") << "  postcore level: " << postCore_.quantity();
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
extern "C" cyclus::Model* constructBatchReactor()
{
  return new BatchReactor();
}

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
extern "C" void destructBatchReactor(cyclus::Model* model)
{
      delete model;
}

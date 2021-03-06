/* banker_account_test.cc
   Jeremy Barnes, 16 November 2012
   Copyright (c) 2012 Datacratic.  All rights reserved.

   Test for Banker accounts.
*/

#define BOOST_TEST_MAIN
#define BOOST_TEST_DYN_LINK

#include <boost/test/unit_test.hpp>
#include "jml/arch/format.h"
#include "jml/arch/exception_handler.h"
#include "jml/utils/guard.h"
#include "rtbkit/core/banker/account.h"
#include "jml/utils/environment.h"
#include <boost/thread/thread.hpp>
#include "jml/arch/atomic_ops.h"
#include "jml/arch/timers.h"
#include "jml/utils/ring_buffer.h"


using namespace std;
using namespace ML;
using namespace Datacratic;
using namespace RTBKIT;

BOOST_AUTO_TEST_CASE( test_account_set_budget )
{
    Account account;

    /* set initial budget */
    account.setBudget(USD(8));
    BOOST_CHECK_EQUAL(account.available, USD(8));
    BOOST_CHECK_EQUAL(account.budgetIncreases, USD(8));
    BOOST_CHECK_EQUAL(account.budgetDecreases, USD(0));

    /* adjust budget down:
       1 usd added to adjustmentsOut (deduced from budget) */
    account.setBudget(USD(7));
    BOOST_CHECK_EQUAL(account.available, USD(7));
    BOOST_CHECK_EQUAL(account.budgetIncreases, USD(8));
    BOOST_CHECK_EQUAL(account.budgetDecreases, USD(1));

    /* adjust budget up:
       1 usd added to adjustmentsIn to balance with adj.Out */
    account.setBudget(USD(8));
    BOOST_CHECK_EQUAL(account.available, USD(8));
    BOOST_CHECK_EQUAL(account.budgetIncreases, USD(9));
    BOOST_CHECK_EQUAL(account.budgetDecreases, USD(1));

    /* adjust budget up:
       3 usd put in budget */
    account.setBudget(USD(13));
    BOOST_CHECK_EQUAL(account.available, USD(13));
    BOOST_CHECK_EQUAL(account.budgetIncreases, USD(14));
    BOOST_CHECK_EQUAL(account.budgetDecreases, USD(1));

    /* negative adjustments must be limited by "available":
       of the previous 13 usd budget, 10 already have been spent, which means
       we cannot go below 10 USD, even though 3 USD are still available
     */
    account.allocatedOut = USD(10);
    account.available = USD(3);
    account.checkInvariants();
    {
        auto notrace = Set_Trace_Exceptions(false);
        BOOST_CHECK_THROW(account.setBudget(USD(9)),
                          std::exception);
    }

    /* we adjust the budget down the the least possible value and ensure that
       "available" is adjusted by taking the "allocatedOut" into account */
    account.setBudget(USD(10));
    BOOST_CHECK_EQUAL(account.available, USD(0));
}

BOOST_AUTO_TEST_CASE( test_account_tojson )
{
    Account account;

    Json::Value testState = Json::parse(
        "{ 'md':  { 'objectType': 'Account',"
        "           'version': 1 },"
        "  'type': 'none',"
        "  'budgetIncreases': {},"
        "  'budgetDecreases': {},"
        "  'spent': {},"
        "  'recycledIn': {},"
        "  'recycledOut': {},"
        "  'allocatedIn': {},"
        "  'allocatedOut': {},"
        "  'commitmentsMade': {},"
        "  'commitmentsRetired': {},"
        "  'adjustmentsIn': {},"
        "  'adjustmentsOut': {},"
        "  'lineItems': {},"
        "  'adjustmentLineItems': {}}");

    /* fresh and clean account */
    BOOST_CHECK_EQUAL(account.toJson(), testState);

    /* account with a 10 USD budget */
    account.setBudget(USD(10));
    testState["budgetIncreases"]["USD/1M"] = 10000000;
    BOOST_CHECK_EQUAL(account.toJson(), testState);
}

BOOST_AUTO_TEST_CASE( test_account_hierarchy )
{
    Account budgetAccount;
    budgetAccount.setBudget(USD(10));

    Account commitmentAccount, spendAccount;

    ShadowAccount shadowCommitmentAccount;
    ShadowAccount shadowSpendAccount;

    commitmentAccount.setAvailable(budgetAccount, USD(2));

    BOOST_CHECK_EQUAL(budgetAccount.available, USD(8));
    BOOST_CHECK_EQUAL(commitmentAccount.available, USD(2));

    shadowCommitmentAccount.syncFromMaster(commitmentAccount);
    shadowSpendAccount.syncFromMaster(spendAccount);

    BOOST_CHECK_EQUAL(shadowCommitmentAccount.available, USD(2));
    BOOST_CHECK_EQUAL(shadowSpendAccount.available, USD(0));


    auto doBidding = [&] ()
        {
            bool auth1 = shadowCommitmentAccount.authorizeBid("ad1", USD(1));
            bool auth2 = shadowCommitmentAccount.authorizeBid("ad2", USD(1));
            bool auth3 = shadowCommitmentAccount.authorizeBid("ad3", USD(1));

            BOOST_CHECK_EQUAL(auth1, true);
            BOOST_CHECK_EQUAL(auth2, true);
            BOOST_CHECK_EQUAL(auth3, false);
    
            Amount detached = shadowCommitmentAccount.detachBid("ad1");
            BOOST_CHECK_EQUAL(detached, USD(1));

            shadowCommitmentAccount.cancelBid("ad2");

            shadowSpendAccount.commitDetachedBid(detached, USD(0.50), LineItems());

            shadowCommitmentAccount.syncToMaster(commitmentAccount);
            shadowSpendAccount.syncToMaster(spendAccount);
        };

    // Do the same kind of bid 5 times
    for (unsigned i = 0;  i < 5;  ++i) {

        doBidding();

        cerr << "budget" << budgetAccount << endl;
        cerr << "spend " << spendAccount << endl;
        cerr << "commitment " << commitmentAccount << endl;
        cerr << "shadow spend" << shadowSpendAccount << endl;
        cerr << "shadow commitment" << shadowCommitmentAccount << endl;

        spendAccount.recuperateTo(budgetAccount);

        cerr << "after recuperation" << endl;
        cerr << "budget" << budgetAccount << endl;
        cerr << "spend " << spendAccount << endl;
   
        commitmentAccount.setAvailable(budgetAccount, USD(2));
        
        cerr << "after setAvailable" << endl;
        cerr << "budget" << budgetAccount << endl;
        cerr << "spend " << spendAccount << endl;
        cerr << "commitment " << commitmentAccount << endl;

        shadowCommitmentAccount.syncFromMaster(commitmentAccount);
        shadowSpendAccount.syncFromMaster(spendAccount);

        cerr << "after sync" << endl;
        cerr << "shadow spend" << shadowSpendAccount << endl;
        cerr << "shadow commitment" << shadowCommitmentAccount << endl;

        BOOST_CHECK_EQUAL(commitmentAccount.available, USD(2));
        BOOST_CHECK_EQUAL(shadowCommitmentAccount.available, USD(2));
        BOOST_CHECK_EQUAL(spendAccount.available, USD(0));
        BOOST_CHECK_EQUAL(shadowSpendAccount.available, USD(0));
    }
}

BOOST_AUTO_TEST_CASE( test_account_recycling )
{
    Accounts accounts;

    AccountKey campaign("campaign");
    AccountKey strategy("campaign:strategy");
    AccountKey strategy2("campaign:strategy2");
    AccountKey spend("campaign:strategy:spend");
    AccountKey spend2("campaign:strategy2:spend");

    accounts.createBudgetAccount(campaign);
    accounts.createBudgetAccount(strategy);
    accounts.createBudgetAccount(strategy2);
    accounts.createSpendAccount(spend);
    accounts.createSpendAccount(spend2);

    // Top level budget of $10
    accounts.setBudget(campaign, USD(10));

    // Make $2 available in the strategy account
    accounts.setAvailable(strategy, USD(2), AT_NONE);
    accounts.setAvailable(strategy2, USD(2), AT_NONE);
    
    BOOST_CHECK_EQUAL(accounts.getAvailable(campaign), USD(6));
    BOOST_CHECK_EQUAL(accounts.getAvailable(strategy), USD(2));
    BOOST_CHECK_EQUAL(accounts.getAvailable(strategy2), USD(2));

    accounts.setAvailable(spend, USD(1), AT_NONE);
    //accounts.setAvailable(spend2, USD(1), AT_NONE);

    BOOST_CHECK_EQUAL(accounts.getAvailable(campaign), USD(6));
    BOOST_CHECK_EQUAL(accounts.getAvailable(strategy), USD(1));
    BOOST_CHECK_EQUAL(accounts.getAvailable(strategy2), USD(2));
    BOOST_CHECK_EQUAL(accounts.getAvailable(spend), USD(1));
    BOOST_CHECK_EQUAL(accounts.getAvailable(spend2), USD(0));

    accounts.setAvailable(spend, USD(1), AT_NONE);
    //accounts.setAvailable(spend2, USD(1), AT_NONE);

    BOOST_CHECK_EQUAL(accounts.getAvailable(campaign), USD(6));
    BOOST_CHECK_EQUAL(accounts.getAvailable(strategy), USD(1));
    BOOST_CHECK_EQUAL(accounts.getAvailable(strategy2), USD(2));
    BOOST_CHECK_EQUAL(accounts.getAvailable(spend), USD(1));
    BOOST_CHECK_EQUAL(accounts.getAvailable(spend2), USD(0));

    accounts.setAvailable(strategy, USD(2), AT_NONE);
    //accounts.setAvailable(strategy2, USD(2), AT_NONE);

    BOOST_CHECK_EQUAL(accounts.getAvailable(campaign), USD(5));
    BOOST_CHECK_EQUAL(accounts.getAvailable(strategy), USD(2));
    BOOST_CHECK_EQUAL(accounts.getAvailable(strategy2), USD(2));
    BOOST_CHECK_EQUAL(accounts.getAvailable(spend), USD(1));
    BOOST_CHECK_EQUAL(accounts.getAvailable(spend2), USD(0));
}

BOOST_AUTO_TEST_CASE( test_accounts )
{
    Accounts accounts;

    AccountKey budget("budget");
    AccountKey commitment("budget:commitment");
    AccountKey spend("budget:spend");

    ShadowAccounts shadow;

    accounts.createBudgetAccount(budget);
    accounts.createSpendAccount(commitment);
    accounts.createSpendAccount(spend);

    // Top level budget of $10
    accounts.setBudget(budget, USD(10));

    // Make $2 available in the commitment account
    accounts.setAvailable(commitment, USD(2), AT_SPEND);
    
    BOOST_CHECK_EQUAL(accounts.getAvailable(budget), USD(8));
    BOOST_CHECK_EQUAL(accounts.getAvailable(commitment), USD(2));

    shadow.activateAccount(commitment);
    shadow.activateAccount(spend);

    auto doBidding = [&] ()
        {
            shadow.syncFrom(accounts);

            bool auth1 = shadow.authorizeBid(commitment, "ad1", USD(1));
            bool auth2 = shadow.authorizeBid(commitment, "ad2", USD(1));
            bool auth3 = shadow.authorizeBid(commitment, "ad3", USD(1));

            BOOST_CHECK_EQUAL(auth1, true);
            BOOST_CHECK_EQUAL(auth2, true);
            BOOST_CHECK_EQUAL(auth3, false);

            shadow.checkInvariants();

            Amount detached = shadow.detachBid(commitment, "ad1");
            BOOST_CHECK_EQUAL(detached, USD(1));

            shadow.checkInvariants();

            shadow.cancelBid(commitment, "ad2");

            shadow.checkInvariants();

            shadow.commitDetachedBid(spend, detached, USD(0.50), LineItems());

            shadow.syncTo(accounts);

            accounts.checkInvariants();

            cerr << accounts.getAccountSummary(budget) << endl;
    
        };

    // Do the same kind of bid 5 times
    for (unsigned i = 0;  i < 5;  ++i) {

        cerr << accounts.getAccountSummary(budget) << endl;
        cerr << accounts.getAccount(budget) << endl;
        cerr << accounts.getAccount(commitment) << endl;
        cerr << accounts.getAccount(spend) << endl;

        doBidding();

        //cerr << "budget" << budgetAccount << endl;
        //cerr << "spend " << spendAccount << endl;
        //cerr << "commitment " << commitmentAccount << endl;
    
        accounts.recuperate(spend);

        accounts.checkInvariants();

        //cerr << "after recuperation" << endl;
        //cerr << "budget" << budgetAccount << endl;
        //cerr << "spend " << spendAccount << endl;
   
        accounts.setAvailable(commitment, USD(2), AT_SPEND);
        
        accounts.checkInvariants();

        //cerr << "after setAvailable" << endl;
        //cerr << "budget" << budgetAccount << endl;
        //cerr << "spend " << spendAccount << endl;
        //cerr << "commitment " << commitmentAccount << endl;
    }

    cerr << accounts.getAccountSummary(budget) << endl;
}

BOOST_AUTO_TEST_CASE( test_multiple_bidder_threads )
{
    Accounts master;

    AccountKey campaign("campaign");
    AccountKey strategy("campaign:strategy");

    // Create a budget for the campaign
    master.createBudgetAccount(strategy);
    master.setBudget(campaign, USD(10));

    // Do 1,000 topup transfers of one micro

    int nTopupThreads = 2;
    int nAddBudgetThreads = 2;
    int nBidThreads = 2; 
    //int nSpendThreads = 2;
    int numTransfersPerThread = 10000;
    int numAddBudgetsPerThread = 10;

    volatile bool finished = false;

    auto runTopupThread = [&] ()
        {
            while (!finished) {
                master.setAvailable(strategy, USD(0.10), AT_BUDGET);
            }
        };

    auto runAddBudgetThread = [&] ()
        {
            for (unsigned i = 0;  i < numAddBudgetsPerThread;  ++i) {
                
                AccountSummary summary = master.getAccountSummary(campaign);
                cerr << summary << endl;
                master.setBudget(campaign, summary.budget + USD(1));

                ML::sleep(1.0);
            }
        };

    uint64_t numBidsCommitted = 0;

    ML::RingBufferSRMW<Amount> toCommitThread(1000000);
    

    auto runBidThread = [&] (int threadNum)
        {
            ShadowAccounts shadow;
            AccountKey account = strategy;
            account.push_back("bid" + to_string(threadNum));

            master.createSpendAccount(account);
            shadow.activateAccount(account);
            shadow.syncFrom(master);

            int done = 0;
            for (;  !finished;  ++done) {
                string item = "item";

                // Every little bit, do a sync and a re-up
                if (done && done % 1000 == 0) {
                    shadow.syncTo(master);
                    master.setAvailable(account, USD(0.10), AT_NONE);
                    shadow.syncFrom(master);
                    //cerr << "done " << done << " bids" << endl;
                }
                
                // Authorize 10
                if (!shadow.authorizeBid(account, item, MicroUSD(1))) {
                    continue;
                }

                // In half of the cases, we cancel.  In the other half, we
                // transfer it off to the commit thread

                if (done % 2 == 0) {
                    // Commit 1
                    shadow.commitBid(account, item, MicroUSD(1), LineItems());
                    ML::atomic_inc(numBidsCommitted);
                }
                else {
                    Amount amount = shadow.detachBid(account, item);
                    toCommitThread.push(amount);
                }
            }

            shadow.sync(master);

            cerr << "finished shadow account with "
                 << done << " bids" << endl;
            cerr << master.getAccount(account) << endl;

        };

    auto runCommitThread = [&] (int threadNum)
        {
            ShadowAccounts shadow;
            AccountKey account = strategy;
            account.push_back("commit" + to_string(threadNum));

            master.createSpendAccount(account);
            shadow.activateAccount(account);
            shadow.syncFrom(master);

            while (!finished || toCommitThread.couldPop()) {
                Amount amount;
                if (toCommitThread.tryPop(amount, 0.1)) {
                    shadow.commitDetachedBid(account, amount, MicroUSD(1), LineItems());
                    ML::atomic_inc(numBidsCommitted);
                }
                shadow.syncTo(master);
            }

            shadow.syncTo(master);
            cerr << "done commit thread" << endl;
        };

    boost::thread_group budgetThreads;

    for (unsigned i = 0;  i < nAddBudgetThreads;  ++i)
        budgetThreads.create_thread(runAddBudgetThread);

    boost::thread_group bidThreads;
    for (unsigned i = 0;  i < nBidThreads;  ++i)
        bidThreads.create_thread(std::bind(runBidThread, i));

    for (unsigned i = 0;  i < nTopupThreads;  ++i)
        bidThreads.create_thread(runTopupThread);

    bidThreads.create_thread(std::bind(runCommitThread, 0));
    

    budgetThreads.join_all();

    finished = true;

    bidThreads.join_all();

    uint32_t amountAdded       = nAddBudgetThreads * numAddBudgetsPerThread;
    uint32_t amountTransferred = nTopupThreads * numTransfersPerThread;

    cerr << "numBidsCommitted = "  << numBidsCommitted << endl;
    cerr << "amountTransferred = " << amountTransferred << endl;
    cerr << "amountAdded =       " << amountAdded << endl;

    cerr << "campaign" << endl;
    cerr << master.getAccountSummary(campaign) << endl;
    cerr << master.getAccount(campaign) << endl; 

    cerr << "strategy" << endl;
    cerr << master.getAccountSummary(strategy) << endl;
    cerr << master.getAccount(strategy) << endl; 


#if 0    
    RedisBanker banker("bankerTest", "b", s, redis);
    banker.sync();
    Json::Value status = banker.getCampaignStatusJson("testCampaign", "");

    cerr << status << endl;




    BOOST_CHECK_EQUAL(status["available"]["micro-USD"].asInt(), 1000000 - amountTransferred + amountAdded);
    BOOST_CHECK_EQUAL(status["strategies"][0]["available"]["micro-USD"].asInt(),
                      amountTransferred - numBidsCommitted);
    BOOST_CHECK_EQUAL(status["strategies"][0]["transferred"]["micro-USD"].asInt(),
                      amountTransferred);
    BOOST_CHECK_EQUAL(status["strategies"][0]["spent"]["micro-USD"].asInt(),
                      numBidsCommitted);
    BOOST_CHECK_EQUAL(status["spent"]["micro-USD"].asInt(), numBidsCommitted);

    //BOOST_CHECK_EQUAL(status["available"].
#endif
}

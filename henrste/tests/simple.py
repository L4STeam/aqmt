#!/usr/bin/env python3

# add path to library root
import sys
import os
sys.path.append(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))

from framework import Testbed, TestEnv, run_test, steps
from framework.plot import collection_components
from framework.traffic import greedy


def test():

    def my_test(testcase):
        testcase.traffic(greedy, node='a', tag='cubic 1')
        #testcase.traffic(greedy, node='a', tag='cubic 2')
        testcase.traffic(greedy, node='b', tag='ecn-cubic 1')
        #testcase.traffic(greedy, node='b', tag='ecn-cubic 2')

    testbed = Testbed()

    testbed.ta_samples = 300
    testbed.ta_idle = 0
    testbed.ta_delay = 125

    testbed.cc('a', 'cubic', testbed.ECN_ALLOW)
    #testbed.cc('b', 'dctcp-drop', testbed.ECN_INITIATE)
    testbed.cc('b', 'cubic', testbed.ECN_INITIATE)

    run_test(
        folder='results/simple',
        title='Just a simple test to verify setup',
        testenv=TestEnv(testbed, retest=False, reanalyze=True),
        steps=(
            steps.plot_compare(swap_levels=[1], components=[
                collection_components.utilization_tags(),
                collection_components.window_rate_ratio(),
                collection_components.queueing_delay(),
                collection_components.drops_marks(),
            ]),
            steps.plot_flows(swap_levels=[1]),
            steps.branch_sched([
                # tag, title, name, params
                #('pi2',
                #    'PI2: dc_dualq dc_ecn target 15ms tupdate 15ms alpha 5 beta 50 k 2 t\\\\_shift 30ms l\\\\_drop 100',
                #    'pi2', 'dc_dualq dc_ecn target 15ms tupdate 15ms alpha 5 beta 50 k 2 t_shift 30ms l_drop 100'),
                ('pie', 'PIE', 'pie', 'ecn target 15ms tupdate 15ms alpha 1 beta 10 ecndrop 25'),
                #('pfifo', 'pfifo', 'pfifo', ''),
            ]),
            steps.branch_rtt([
                2,
                10,
                50,
                #100,
            ], title='%d'),
            steps.branch_bitrate([
                10,
                100,
            ]),
            my_test,
        )
    )

if __name__ == '__main__':
    test()

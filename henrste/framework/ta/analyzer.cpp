#include "analyzer.h"

#include <csignal>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/if_ether.h> /* includes net/ethernet.h */
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <iostream>
#include <map>
#include <unistd.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <strings.h>
#include <fstream>
#include <sys/stat.h>
#include <vector>
#include <math.h>
#include <array>
#include <time.h>
#include <sys/types.h>

#define DEMO_QLIM 140
#define DEMO_RTT 0.007
#define NSEC_PER_SEC 1000000000UL
#define NSEC_PER_MS 1000000UL
#define US_PER_S 1000000UL
#define NSEC_PER_US 1000UL

#define percentile(p, n) (ceil(float(p)/100*float(n)))

struct RateVar {
public:
    RateVar(uint32_t r, double c) {
        rate = r;
        cv = c;
    }
    uint32_t rate;
    double cv;
};

static void *pcapLoop(void *param);
static void *printInfo(void *param);

uint64_t getStamp()
{
    // returns us
    struct timespec monotime;
    clock_gettime(CLOCK_MONOTONIC, &monotime);
    return ((uint64_t)monotime.tv_sec) * US_PER_S + monotime.tv_nsec / NSEC_PER_US;
}

DemoData::DemoData()
{
    ipclass = 0;
    pthread_mutexattr_t errorcheck;
    pthread_mutexattr_init(&errorcheck);
    pthread_mutex_init(&mutex, &errorcheck);
    pthread_cond_init(&newdata, NULL);
    init();
    linkcap = 5000000;
    rtt_ecn = 0;
    rtt_nonecn = 0;
    rtt_base = 0.007;
}

void DemoData::init()
{
    util = 0;
    mark_ecn = 0;
    drop_ecn = 0;
    mark_nonecn = 0;
    drop_nonecn = 0;
    fair_rate = 0;
    fair_window = 0;
    alrate_ecn = 0;
    alrate_nonecn = 0;
    alw_ecn = 0;
    alw_nonecn = 0;
    cbrrate_ecn = 0;
    cbrrate_nonecn = 0;

    ecn_th.clear();
    nonecn_th.clear();
    ecn_w.clear();
    nonecn_w.clear();

    ecn_th.resize(10);
    nonecn_th.resize(10);
    ecn_w.resize(10);
    nonecn_w.resize(10);
    avg_qsize_ll = 0;
    avg_qsize_c = 0;
    p99_qsize_ll = 0;
    p99_qsize_c = 0;
    ll_qsize_y.clear();
    c_qsize_y.clear();
    ll_qsize_y.resize(DEMO_QLIM);
    c_qsize_y.resize(DEMO_QLIM);
}

ThreadParam::ThreadParam(pcap_t *descr, uint32_t sinterval, char *folder, uint32_t nrs, bool ipc)
: ipclass(ipc)
{
    db1 = new DataBlock();
    db2 = new DataBlock();
    db1->init();
    db1->start = getStamp();

    m_descr = descr;

    pthread_mutexattr_t errorcheck;
    pthread_mutexattr_init(&errorcheck);
    pthread_mutex_init(&m_mutex, &errorcheck);
    m_sinterval = sinterval;
    m_folder = folder;
    m_nrs = nrs;

    nr_ecn_flows = 0;
    nr_nonecn_flows = 0;

    packets_captured = 0;
    packets_processed = 0;

    quit = false;
    sample_id = 0;
}

void ThreadParam::swapDB(){ // called by printInfo
    // db2 is initialized already
    pthread_mutex_lock(&m_mutex);
    DataBlock *tmp = db1;
    db1 = db2;
    db2->start = getStamp();
    tmp->last = db1->start;
    pthread_mutex_unlock(&m_mutex);
    db2 = tmp;
}

// we need ThreadParam global to use it in the signal handler
ThreadParam *tp;

void signalHandler(int signum) {
    tp->quit = true;
    pthread_cond_broadcast(&tp->quit_cond);
}

std::string IPtoString(in_addr_t ip) {
    struct sockaddr_in ipip;
    ipip.sin_addr.s_addr = ip;
    return std::string(inet_ntoa(ipip.sin_addr));
}

void processPacket(u_char *, const struct pcap_pkthdr *header, const u_char *buffer)
{
    struct iphdr *iph = (struct iphdr*)(buffer + 14); // ethernet header is 14 bytes

    uint8_t proto = iph->protocol;
    uint16_t sport = 0;
    uint16_t dport = 0;
    uint16_t id = ntohs(iph->id);
    uint16_t qsize = id & 2047;
    double tmp_qs = (double) qsize; // * 1.049318; // fix the error caused by bit shifting instead of division
    qsize = (uint16_t)round(tmp_qs);
    uint16_t drops = (id >> 11);

    if (proto == IPPROTO_TCP) {
        struct tcphdr *tcph = (struct tcphdr*)(buffer + 14 + iph->ihl*4);
        sport = ntohs(tcph->source);
        dport = ntohs(tcph->dest);
    } else if (proto == IPPROTO_UDP) {
        struct udphdr *udph = (struct udphdr*) (buffer + 14 + iph->ihl*4);
        sport = ntohs(udph->source);
        dport = ntohs(udph->dest);
    }

    SrcDst sd(proto, iph->saddr, sport, iph->daddr, dport);
    uint64_t iplen = ntohs(iph->tot_len) + 14; // include the 14 bytes in ethernet header
                                       // the link bandwidth includes it
    iplen *= 8; // use bits
    std::map<SrcDst,FlowData> *fmap;
    uint32_t mark = 0;

    if (qsize >= QS_LIMIT)
        qsize = QS_LIMIT-1;

    uint8_t ts = iph->tos;
    if ((ts & 3) == 3)
        mark = 1;

    if (tp->ipclass)
        ts = ntohl(iph->saddr);

    pthread_mutex_lock(&tp->m_mutex);

    switch (ts & 3) {
    case 0:
        tp->db1->tot_packets_nonecn++;
        tp->db1->qs.ecn00[qsize]++;
        tp->db1->d_qs.ecn00[qsize]+= drops;
        fmap = &tp->db1->fm.nonecn_rate;
        break;
    case 1:
        tp->db1->tot_packets_ecn++;
        tp->db1->qs.ecn01[qsize]++;
        tp->db1->d_qs.ecn01[qsize]+= drops;
        fmap = &tp->db1->fm.ecn_rate;
        break;
    case 2:
        tp->db1->tot_packets_ecn++;
        tp->db1->qs.ecn10[qsize]++;
        tp->db1->d_qs.ecn10[qsize]+= drops;
        fmap = &tp->db1->fm.ecn_rate;
        break;
    case 3:
        tp->db1->tot_packets_ecn++;
        tp->db1->qs.ecn11[qsize]++;
        tp->db1->d_qs.ecn11[qsize]+= drops;
        fmap = &tp->db1->fm.ecn_rate;
        break;
    }

    uint32_t idport;
    if (dport == 22 || (dport > 5000 && dport <= 5050))
        idport = dport;
    else
        idport = sport;

    std::pair<std::map<SrcDst,FlowData>::iterator,bool> ret;
    ret = fmap->insert(std::pair<SrcDst,FlowData>(sd, FlowData((uint64_t)iplen, (uint32_t)drops, mark, idport)));
    if (ret.second == false)
        fmap->at(sd).update(iplen, drops, mark);

    tp->packets_captured++;
    pthread_mutex_unlock(&tp->m_mutex);
}

std::ofstream *openFileW(std::string folder, std::string filename)
{
    std::string filename_out = folder + filename;
    std::ofstream *f = new std::ofstream(filename_out.c_str());
    if (!f->is_open())
        std::cerr << "error opening file for writing: " << filename_out << std::endl;
    return f;
}

std::string getProtoRepr(uint8_t proto) {
    if (proto == IPPROTO_TCP)
        return "TCP";
    else if (proto == IPPROTO_UDP)
        return "UDP";
    else if (proto == IPPROTO_ICMP)
        return "ICMP";
    return "UNKNOWN";
}

void printStreamInfo(SrcDst sd)
{
    std::cout << getProtoRepr(sd.m_proto) << " " << IPtoString(sd.m_srcip) << ":" << sd.m_srcport << " -> ";
    std::cout << IPtoString(sd.m_dstip) << ":" << sd.m_dstport;
}

int start_analysis(char *dev, char *folder, uint32_t sinterval, std::string &pcapfilter, bool ipclass, uint32_t nrs)
{
    int i;
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *descr;
    const u_char *packet;
    struct pcap_pkthdr hdr;     /* pcap.h */
    struct ether_header *eptr;  /* net/ethernet.h */
    struct bpf_program fp;      /* The compiled filter expression */
    bpf_u_int32 mask;       /* The netmask of our sniffing device */
    bpf_u_int32 net;        /* The IP of our sniffing device */
    u_char *ptr; /* printing out hardware header info */
    char *srcnet;

    if (pcap_lookupnet(dev, &net, &mask, errbuf) == -1) {
        fprintf(stderr, "Can't get netmask for device %s\n", dev);
        net = 0;
        mask = 0;
    }

    descr = pcap_open_live(dev, BUFSIZ, 0, 1, errbuf);

    if (descr == NULL) {
        printf("pcap_open_live(): %s\n", errbuf);
        exit(1);
    }

    if (pcap_compile(descr, &fp, pcapfilter.c_str(), 0, net) == -1) {
        fprintf(stderr, "Couldn't parse filter: %s\n", pcap_geterr(descr));
        return(2);
    }

    if (pcap_setfilter(descr, &fp) == -1) {
        fprintf(stderr, "Couldn't install filter: %s\n", pcap_geterr(descr));
        return(2);
    }

    mkdir(folder, 0777);

    pthread_t thread_id[2];
    pthread_attr_t attrs;
    pthread_attr_init(&attrs);
    pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_JOINABLE);
    int res;
    tp = new ThreadParam(descr, sinterval, folder, nrs, ipclass);

    // initialize pthread condition used to quit threads on interrupts
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&tp->quit_cond, &attr);
    pthread_mutex_init(&tp->quit_lock, NULL);

    thread_id[0] = 0;
    res = pthread_create(&thread_id[0], &attrs, &pcapLoop, NULL);

    if (res != 0) {
        fprintf(stderr, "Error while creating thread, exiting...\n");
        exit(1);
    }

    thread_id[1] = 0;
    pthread_create(&thread_id[1], &attrs, &printInfo, NULL);

    if (res != 0) {
        fprintf(stderr, "Error while creating thread, exiting...\n");
        exit(1);
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    pthread_join(thread_id[1], NULL);
    pcap_breakloop(tp->m_descr);
    pthread_join(thread_id[0], NULL);

    std::cout << "Packets captured: " << tp->packets_captured << std::endl;
    std::cout << "Packets processed: " << tp->packets_processed << std::endl;

    return 0;
}

void *pcapLoop(void *)
{
    // Put the device in sniff loop
    //fprintf(stderr, "starting capture\n");
    pcap_loop(tp->m_descr, -1, processPacket, NULL);
    pcap_close(tp->m_descr);
    //fprintf(stderr, "pcap_loop returned\n");
    return 0;
}

void addFlow(std::map<SrcDst,std::vector<FlowData>> *fd_pf, SrcDst srcdst, FlowData fd) {
    uint64_t samplelen = tp->db2->last - tp->db2->start;
    uint64_t r = fd.rate * 1000000 / samplelen;

    printStreamInfo(srcdst);
    printf(" %lu bits/sec\n", r);

    if (srcdst.m_proto == IPPROTO_TCP || srcdst.m_proto == IPPROTO_UDP || srcdst.m_proto == IPPROTO_ICMP) {
        if (fd_pf->count(srcdst) == 0) {
            std::vector<FlowData> data;
            data.resize(tp->sample_id);
            fd_pf->insert(std::pair<SrcDst,std::vector<FlowData>>(srcdst, data));
        }

        if (fd_pf->at(srcdst).size() != tp->sample_id) {
            throw "Sample ID not equal to flow map size";
        }

        fd.rate = r;
        fd_pf->at(srcdst).push_back(fd);
    }
}

void processFD()
{
    printf("Throughput per stream (ECN queue):\n");

    for (auto& kv: tp->db2->fm.ecn_rate) {
        const SrcDst& srcdst = kv.first;
        FlowData& fd = kv.second;

        addFlow(&tp->fd_pf_ecn, srcdst, fd);
    }

    printf("Throughput per stream (non-ECN queue):\n");

    for (auto& kv: tp->db2->fm.nonecn_rate) {
        const SrcDst& srcdst = kv.first;
        FlowData& fd = kv.second;

        addFlow(&tp->fd_pf_nonecn, srcdst, fd);
    }

    // make sure all lists are filled
    for (auto& kv: tp->fd_pf_ecn) {
        if (kv.second.size() != tp->sample_id + 1) {
            kv.second.resize(tp->sample_id + 1);
        }
    }
    for (auto& kv: tp->fd_pf_nonecn) {
        if (kv.second.size() != tp->sample_id + 1) {
            kv.second.resize(tp->sample_id + 1);
        }
    }
}

void wait(uint64_t sleep_ns) {
    struct timespec target;
    clock_gettime(CLOCK_MONOTONIC, &target);

    uint64_t nsec = target.tv_nsec + sleep_ns;
    while (nsec > NSEC_PER_SEC) {
        target.tv_sec++;
        nsec -= NSEC_PER_SEC;
    }

    target.tv_nsec = nsec;

    pthread_mutex_lock(&tp->quit_lock);
    int ret = 0;

    while (ret == 0 && !tp->quit)
        ret = pthread_cond_timedwait(&tp->quit_cond, &tp->quit_lock, &target);

    pthread_mutex_unlock(&tp->quit_lock);
}

void *printInfo(void *)
{
    uint16_t qsize_reno;
    uint16_t qsize_dctcp;

    uint64_t qs_pdf_ecn[QS_LIMIT];
    uint64_t qs_pdf_nonecn[QS_LIMIT];
    uint64_t drops_pdf_ecn[QS_LIMIT];
    uint64_t drops_pdf_nonecn[QS_LIMIT];
    bzero(qs_pdf_ecn, sizeof(uint64_t)*QS_LIMIT);
    bzero(qs_pdf_nonecn, sizeof(uint64_t)*QS_LIMIT);
    bzero(drops_pdf_ecn, sizeof(uint64_t)*QS_LIMIT);
    bzero(drops_pdf_nonecn, sizeof(uint64_t)*QS_LIMIT);

    uint64_t time_ms;

    /* queue size */
    // per sample
    printf("Output folder: %s\n", tp->m_folder);

    std::ofstream *f_tot_packets_ecn = openFileW(tp->m_folder, "/tot_packets_ecn");
    std::ofstream *f_tot_packets_nonecn = openFileW(tp->m_folder, "/tot_packets_nonecn");
    std::ofstream *f_qs_ecn_pdf00s = openFileW(tp->m_folder, "/qs_ecn00_s");
    std::ofstream *f_qs_ecn_pdf01s = openFileW(tp->m_folder, "/qs_ecn01_s");
    std::ofstream *f_qs_ecn_pdf10s = openFileW(tp->m_folder, "/qs_ecn10_s");
    std::ofstream *f_qs_ecn_pdf11s = openFileW(tp->m_folder, "/qs_ecn11_s");
    std::ofstream *f_qs_ecn_pdfsums = openFileW(tp->m_folder, "/qs_ecnsum_s");

    std::ofstream *f_qs_ecn_avg = openFileW(tp->m_folder, "/qs_ecn_avg");
    std::ofstream *f_qs_nonecn_avg = openFileW(tp->m_folder, "/qs_nonecn_avg");

    /* per sample total of rate, drops, marks */
    std::ofstream *f_r_tot_ecn =  openFileW(tp->m_folder, "/r_tot_ecn");
    std::ofstream *f_r_tot_nonecn =  openFileW(tp->m_folder, "/r_tot_nonecn");
    std::ofstream *f_d_tot_ecn =  openFileW(tp->m_folder, "/d_tot_ecn");
    std::ofstream *f_d_tot_nonecn =  openFileW(tp->m_folder, "/d_tot_nonecn");
    std::ofstream *f_m_tot_ecn =  openFileW(tp->m_folder, "/m_tot_ecn");
    std::ofstream *f_r_tot =  openFileW(tp->m_folder, "/r_tot");

    *f_qs_ecn_pdf00s << PLOT_MATRIX_DIM;
    *f_qs_ecn_pdf01s << PLOT_MATRIX_DIM;
    *f_qs_ecn_pdf10s << PLOT_MATRIX_DIM;
    *f_qs_ecn_pdf11s << PLOT_MATRIX_DIM;
    *f_qs_ecn_pdfsums << PLOT_MATRIX_DIM;

    for (int i = 0; i < QS_LIMIT; ++i) {
        *f_qs_ecn_pdf00s << " " << i;
        *f_qs_ecn_pdf01s << " " << i;
        *f_qs_ecn_pdf10s << " " << i;
        *f_qs_ecn_pdf11s << " " << i;
        *f_qs_ecn_pdfsums << " " << i;
    }

    *f_qs_ecn_pdf00s << std::endl;
    *f_qs_ecn_pdf01s << std::endl;
    *f_qs_ecn_pdf10s << std::endl;
    *f_qs_ecn_pdf11s << std::endl;
    *f_qs_ecn_pdfsums << std::endl;

    // first run
    // to get accurate results we swap the database and initialize timers here
    // (this way we don't time wrong and gets packets outside our time area)
    tp->db2->init();
    tp->swapDB();
    tp->start = tp->db1->start;

    wait(tp->m_sinterval * NSEC_PER_MS);

    uint64_t elapsed, next, sleeptime;

    while (1) {
        tp->swapDB();

        std::vector<uint32_t> rvec_ecn;
        std::vector<uint32_t> rvec_nonecn;
        uint64_t recn_tot_s = 0;
        uint64_t rnonecn_tot_s = 0;

        /* queue size, drops as a function queue size */
        // total for the experiment, all samples summed up
        std::ofstream *f_qs_drops_ecn_pdf = openFileW(tp->m_folder, "/qs_drops_ecn_pdf");
        std::ofstream *f_qs_drops_nonecn_pdf = openFileW(tp->m_folder, "/qs_drops_nonecn_pdf");

        // CDF
        std::ofstream *f_qs_drops_ecn_cdf = openFileW(tp->m_folder,  "/qs_drops_ecn_cdf");
        std::ofstream *f_qs_drops_nonecn_cdf = openFileW(tp->m_folder,  "/qs_drops_nonecn_cdf");

        // time since we started processing
        time_ms = (tp->db2->last - tp->start) / 1000;
        tp->sample_times.push_back(time_ms);

        printf("\n--- SAMPLE # %d", (int) tp->sample_id + 1);
        if (tp->m_nrs != 0) {
            printf(" of %d", tp->m_nrs);
        }
        printf(" -- total run time %d ms ---\n", (int) time_ms);

        printf("       ECN 00 qsize: ");
        printf("       ECN 01 qsize: ");
        printf("       ECN 10 qsize: ");
        printf("       ECN 11 qsize: \n");

        *f_qs_ecn_pdf00s << time_ms;
        *f_qs_ecn_pdf01s << time_ms;
        *f_qs_ecn_pdf10s << time_ms;
        *f_qs_ecn_pdf11s << time_ms;
        *f_qs_ecn_pdfsums << time_ms;

        uint64_t cdf_qs_ecn = 0;
        uint64_t cdf_qs_nonecn = 0;
        uint64_t cdf_drops_ecn = 0;
        uint64_t cdf_drops_nonecn = 0;

        double qs_ecn_sum = 0;
        double qs_nonecn_sum = 0;
        double nr_samples_ecn = 0;
        double nr_samples_nonecn = 0;
        for (int i = 0; i < QS_LIMIT; ++i) {
            uint64_t ecnsum = 0;
            uint64_t ecndrops = 0;

            if (tp->db2->qs.ecn00[i] > 0 || tp->db2->qs.ecn01[i] > 0 || tp->db2->qs.ecn10[i] > 0 || tp->db2->qs.ecn11[i] > 0) {
                printf("%3d:%10d %20d %20d %20d\n", i, tp->db2->qs.ecn00[i], tp->db2->qs.ecn01[i], tp->db2->qs.ecn10[i], tp->db2->qs.ecn11[i]);
                ecnsum = tp->db2->qs.ecn01[i] + tp->db2->qs.ecn10[i] + tp->db2->qs.ecn11[i];
                qs_pdf_ecn[i] += ecnsum;
                qs_pdf_nonecn[i] += tp->db2->qs.ecn00[i];
                qs_ecn_sum += (ecnsum*i);
                nr_samples_ecn += ecnsum;
                qs_nonecn_sum += (tp->db2->qs.ecn00[i]*i);
                nr_samples_nonecn += tp->db2->qs.ecn00[i];

                ecndrops = tp->db2->d_qs.ecn01[i] + tp->db2->d_qs.ecn10[i] + tp->db2->d_qs.ecn11[i];
                drops_pdf_ecn[i] += ecndrops;
                drops_pdf_nonecn[i] += tp->db2->d_qs.ecn00[i];
            }

            if (tp->sample_id < PLOT_MATRIX_DIM) {
                *f_qs_ecn_pdf00s << " " << tp->db2->qs.ecn00[i];
                *f_qs_ecn_pdf01s << " " << tp->db2->qs.ecn01[i];
                *f_qs_ecn_pdf10s << " " << tp->db2->qs.ecn10[i];
                *f_qs_ecn_pdf11s << " " << tp->db2->qs.ecn11[i];
                *f_qs_ecn_pdfsums << " " << ecnsum;
            }

            // overwritten at each while loop iteration
            *f_qs_drops_ecn_pdf << qs_pdf_ecn[i] << " " << drops_pdf_ecn[i] << std::endl;
            *f_qs_drops_nonecn_pdf << qs_pdf_nonecn[i] << " " << drops_pdf_nonecn[i] << std::endl;
            cdf_qs_ecn += qs_pdf_ecn[i];
            cdf_qs_nonecn += qs_pdf_nonecn[i];
            cdf_drops_ecn += drops_pdf_ecn[i];
            cdf_drops_nonecn += drops_pdf_nonecn[i];
            *f_qs_drops_ecn_cdf << cdf_qs_ecn << " " << cdf_drops_ecn << std::endl;
            *f_qs_drops_nonecn_cdf << cdf_qs_nonecn << " " << cdf_drops_nonecn << std::endl;
        }

        f_qs_drops_ecn_pdf->close();
        f_qs_drops_nonecn_pdf->close();
        f_qs_drops_ecn_cdf->close();
        f_qs_drops_nonecn_cdf->close();

        double qs_ecn_avg = 0;
        double qs_nonecn_avg = 0;
        if (nr_samples_ecn > 0)
            qs_ecn_avg = qs_ecn_sum/nr_samples_ecn;
        if (nr_samples_nonecn > 0)
            qs_nonecn_avg = qs_nonecn_sum/nr_samples_nonecn;

        *f_qs_ecn_avg << (double)time_ms/1000.0 << " " << qs_ecn_avg <<  std::endl;
        *f_qs_nonecn_avg << (double)time_ms/1000.0 << " " << qs_nonecn_avg << std::endl;

        *f_qs_ecn_pdf00s << std::endl;
        *f_qs_ecn_pdf01s << std::endl;
        *f_qs_ecn_pdf10s << std::endl;
        *f_qs_ecn_pdf11s << std::endl;
        *f_qs_ecn_pdfsums << std::endl;

        *f_r_tot_ecn << tp->sample_id << " " << time_ms;
        *f_r_tot_nonecn << tp->sample_id << " " << time_ms;
        *f_d_tot_ecn << tp->sample_id << " " << time_ms;
        *f_d_tot_nonecn << tp->sample_id << " " << time_ms;
        *f_m_tot_ecn << tp->sample_id << " " << time_ms;
        *f_r_tot << tp->sample_id << " " << time_ms;

        processFD();
        uint64_t r_ecn_tot = 0;
        uint64_t r_nonecn_tot = 0;
        uint64_t d_ecn_tot = 0;
        uint64_t d_nonecn_tot = 0;
        uint64_t m_ecn_tot = 0;

        for (auto const& val: tp->fd_pf_ecn) {
            r_ecn_tot += val.second.at(tp->sample_id).rate;
            d_ecn_tot += val.second.at(tp->sample_id).drops;
            m_ecn_tot += val.second.at(tp->sample_id).marks;
        }

        *f_r_tot_ecn << " " << r_ecn_tot;
        *f_d_tot_ecn << " " << d_ecn_tot;
        *f_m_tot_ecn << " " << m_ecn_tot;

        for (auto const& val: tp->fd_pf_nonecn) {
            r_nonecn_tot += val.second.at(tp->sample_id).rate;
            d_nonecn_tot += val.second.at(tp->sample_id).drops;
        }

        *f_r_tot_nonecn << " " << r_nonecn_tot;
        *f_d_tot_nonecn << " " << d_nonecn_tot;

        *f_r_tot << " " << (r_ecn_tot + r_nonecn_tot);

        *f_r_tot << std::endl;
        *f_r_tot_ecn << std::endl;
        *f_r_tot_nonecn << std::endl;
        *f_d_tot_ecn << std::endl;
        *f_d_tot_nonecn << std::endl;
        *f_m_tot_ecn << std::endl;

        *f_tot_packets_ecn << tp->db2->tot_packets_ecn << std::endl;
        *f_tot_packets_nonecn << tp->db2->tot_packets_nonecn << std::endl;

        tp->packets_processed += tp->db2->tot_packets_nonecn + tp->db2->tot_packets_ecn;

        printf("Total throughput: %lu bits/sec\n", (r_nonecn_tot + r_ecn_tot));

        if (tp->m_nrs != 0 && tp->sample_id >= (tp->m_nrs - 1)) {
            printf("Obtained given number of samples (%d)\n", tp->m_nrs);
            break;
        }

        tp->db2->init(); // init outside the critical area to save time

        elapsed = getStamp() - tp->start;
        next = ((uint64_t) tp->sample_id + 2) * tp->m_sinterval * 1000; // convert ms to us

        int process_time = getStamp() - tp->db2->last;
        if (elapsed < next) {
            uint64_t sleeptime = next - elapsed;
            printf("Processed data in approx. %d us - sleeping for %d us\n", (int) process_time, (int) sleeptime);
            wait(sleeptime * NSEC_PER_US);
        }

        if (tp->quit) {
            break;
        }

        tp->sample_id++;
    }

    f_qs_ecn_pdf00s->close();
    f_qs_ecn_pdf01s->close();
    f_qs_ecn_pdf10s->close();
    f_qs_ecn_pdf11s->close();
    f_qs_ecn_pdfsums->close();
    f_tot_packets_ecn->close();
    f_tot_packets_nonecn->close();

    f_r_tot_ecn->close();
    f_r_tot_nonecn->close();
    f_d_tot_ecn->close();
    f_d_tot_nonecn->close();
    f_m_tot_ecn->close();
    f_r_tot->close();

    f_qs_ecn_avg->close();
    f_qs_nonecn_avg->close();

    // write per flow stats
    // (we wait till here because we don't know how many
    //  flows there are before the test is finished)
    std::ofstream *f_r_pf_ecn    =  openFileW(tp->m_folder, "/r_pf_ecn");
    std::ofstream *f_r_pf_nonecn =  openFileW(tp->m_folder, "/r_pf_nonecn");
    std::ofstream *f_d_pf_ecn    =  openFileW(tp->m_folder, "/d_pf_ecn");
    std::ofstream *f_d_pf_nonecn =  openFileW(tp->m_folder, "/d_pf_nonecn");
    std::ofstream *f_m_pf_ecn    =  openFileW(tp->m_folder, "/m_pf_ecn");

    // note: drop and mark numbers per flow don't really tell us much, as
    //       the numbers include whichever packet was handled before this
    //       in the same queue
    //       e.g. a drop might be for another flow

    for (int i = 0; i < tp->sample_times.size(); i++) {
        *f_r_pf_ecn << i << " " << tp->sample_times[i];
        *f_d_pf_ecn << i << " " << tp->sample_times[i];
        *f_m_pf_ecn << i << " " << tp->sample_times[i];

        *f_r_pf_nonecn << i << " " << tp->sample_times[i];
        *f_d_pf_nonecn << i << " " << tp->sample_times[i];

        for (auto const& kv: tp->fd_pf_ecn) {
            *f_r_pf_ecn << " " << kv.second.at(i).rate;
            *f_d_pf_ecn << " " << kv.second.at(i).drops;
            *f_m_pf_ecn << " " << kv.second.at(i).marks;
        }

        for (auto const& kv: tp->fd_pf_nonecn) {
            *f_r_pf_nonecn << " " << kv.second.at(i).rate;
            *f_d_pf_nonecn << " " << kv.second.at(i).drops;
        }

        *f_r_pf_ecn << std::endl;
        *f_d_pf_ecn << std::endl;
        *f_m_pf_ecn << std::endl;

        *f_r_pf_nonecn << std::endl;
        *f_d_pf_nonecn << std::endl;
    }

    f_r_pf_ecn->close();
    f_r_pf_nonecn->close();
    f_d_pf_ecn->close();
    f_d_pf_nonecn->close();
    f_m_pf_ecn->close();

    // save flow details
    std::ofstream *f_flows_ecn    =  openFileW(tp->m_folder, "/flows_ecn");
    std::ofstream *f_flows_nonecn =  openFileW(tp->m_folder, "/flows_nonecn");

    for (auto const& kv: tp->fd_pf_ecn) {
        *f_flows_ecn << getProtoRepr(kv.first.m_proto) << " " << IPtoString(kv.first.m_srcip) << " " << kv.first.m_srcport << " " << IPtoString(kv.first.m_dstip) << " " << kv.first.m_dstport << std::endl;
    }

    for (auto const& kv: tp->fd_pf_nonecn) {
        *f_flows_nonecn << getProtoRepr(kv.first.m_proto) << " " << IPtoString(kv.first.m_srcip) << " " << kv.first.m_srcport << " " << IPtoString(kv.first.m_dstip) << " " << kv.first.m_dstport << std::endl;
    }

    f_flows_ecn->close();
    f_flows_nonecn->close();

    return 0;
}

void usage(int argc, char* argv[])
{
    printf("Usage: %s <dev> <pcap filter exp> <output folder> <sample interval (ms)> (optional:) <ip classification> <nrsamples>\n", argv[0]);
    printf("pcap filter: what to capture. ex.: \"ip and src net 10.187.255.0/24\"\n");
    printf("If nrsamples is not specified, the samples will be recorded until interrupted\nIp classification is t/f (default f); the 2 lsbits of the src ip are used.\n");
    exit(1);
}

int main(int argc, char **argv)
{
    char *dev;
    char *folder;
    uint32_t sinterval;
    uint32_t nrs = 0;
    bool ipclass = false;

    if (argc < 5)
        usage(argc, argv);

    dev = argv[1];

    folder = argv[3];
    sinterval = atoi(argv[4]);
    std::string pcapfilter = argv[2];

    std::cout << "pcap filter: " << pcapfilter << std::endl;

    if (argc > 5)
        ipclass = (argv[5][0] == 't');

    if (argc > 6)
        nrs = atoi(argv[6]);

    start_analysis(dev, folder, sinterval, pcapfilter, ipclass, nrs);

    //fprintf(stderr,"main exiting..\n");
    return 0;
}
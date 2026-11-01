#include <stdio.h>
#include <pcap.h>

int main(void)
{
    pcap_if_t* alldevs;
    pcap_if_t* dev;
    char errbuf[PCAP_ERRBUF_SIZE];

    if (pcap_findalldevs(&alldevs, errbuf) == -1)
    {
        printf("pcap_findalldevs failed: %s\n", errbuf);
        return 1;
    }

    int index = 0;

    for (dev = alldevs; dev != NULL; dev = dev->next)
    {
        printf("[%d]\n", index++);
        printf("Name: %s\n", dev->name);

        if (dev->description != NULL)
            printf("Description: %s\n", dev->description);
        else
            printf("Description: (none)\n");

        printf("\n");
    }

    pcap_freealldevs(alldevs);

    return 0;
}
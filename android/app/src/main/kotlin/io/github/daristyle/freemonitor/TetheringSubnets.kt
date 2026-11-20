package io.github.daristyle.freemonitor

import android.content.Context
import android.net.ConnectivityManager
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.SocketException
import java.nio.ByteBuffer

class TetheringSubnets(
    private val subnets: List<IPv4Subnet>,
) {
    fun contains(address: Int): Boolean = subnets.any { it.contains(address) }

    companion object {
        val NONE = TetheringSubnets(emptyList())

        fun find(context: Context): TetheringSubnets {
            val connectivityManager = context.getSystemService(ConnectivityManager::class.java) ?: return NONE

            @Suppress("DEPRECATION")
            val networks = connectivityManager.allNetworks
            val ownInterfaces = networks.mapNotNull { connectivityManager.getLinkProperties(it)?.interfaceName }.toSet()
            return try {
                val interfaces = NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
                TetheringSubnets(
                    interfaces
                        .filter { it.isUp && !it.isLoopback && it.name !in ownInterfaces }
                        .flatMap { it.interfaceAddresses }
                        .filter { it.address is Inet4Address && it.broadcast != null }
                        .map { IPv4Subnet(ByteBuffer.wrap(it.address.address).int, it.networkPrefixLength.toInt()) },
                )
            } catch (error: SocketException) {
                NONE
            }
        }
    }
}

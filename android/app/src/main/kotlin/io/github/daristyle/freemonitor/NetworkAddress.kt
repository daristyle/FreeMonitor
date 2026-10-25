package io.github.daristyle.freemonitor

import android.content.Context
import android.net.ConnectivityManager
import java.net.Inet4Address

object NetworkAddress {
    fun localIPAddress(context: Context): String? {
        val connectivityManager = context.getSystemService(ConnectivityManager::class.java) ?: return null
        val network = connectivityManager.activeNetwork ?: return null
        val properties = connectivityManager.getLinkProperties(network) ?: return null
        return properties.linkAddresses
            .map { it.address }
            .firstOrNull { it is Inet4Address && !it.isLoopbackAddress }
            ?.hostAddress
    }
}
